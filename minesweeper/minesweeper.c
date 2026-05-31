#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #include <windows.h>
    #include <conio.h>
#else
    #define _GNU_SOURCE
    #include <unistd.h>
    #include <termios.h>
    #include <sys/select.h>
    #include <limits.h>
    #include <libgen.h>
#endif

/* ------------------------------------------------------------------ */
/* MACROS, CONSTANTS & CONFIGURATION                                 */
/* ------------------------------------------------------------------ */
/* Cell Logic Flags */
#define CELL_MINE             0x01
#define CELL_REVEALED         0x02
#define CELL_FLAGGED          0x04

/* Special Layout Coordinates for Incremental Rendering */
#define DIRTY_HEADER          (-1)
#define DIRTY_STATUS          (-2)

/* UI Window Anchors */
#define BOARD_X               2
#define BOARD_Y               3

/* Graphic Presentation Characters */
#ifdef _WIN32
    #define CH_TL             "\xC9"  /* ╔ */
    #define CH_TR             "\xBB"  /* ╗ */
    #define CH_BL             "\xC8"  /* ╚ */
    #define CH_BR             "\xBC"  /* ╝ */
    #define CH_HBAR           "\xCD"  /* ═ */
    #define CH_VBAR           "\xBA"  /* ║ */
#else
    #define CH_TL             "\xe2\x95\x94"  /* ╔ */
    #define CH_TR             "\xe2\x95\x97"  /* ╗ */
    #define CH_BL             "\xe2\x95\x9a"  /* ╚ */
    #define CH_BR             "\xe2\x95\x9d"  /* ╝ */
    #define CH_HBAR           "\xe2\x95\x90"  /* ═ */
    #define CH_VBAR           "\xe2\x95\x91"  /* ║ */
#endif

#define CH_FLAG               "!"
#define CH_MINE               "*"
#define CH_EMPTY              "."

/* Terminal Color Palette Tokens */
enum {
    TERM_COL_DEFAULT,
    TERM_COL_BORDER,
    TERM_COL_TITLE,
    TERM_COL_FLAG,
    TERM_COL_MINE,
    TERM_COL_HIDDEN,
    TERM_COL_WIN,
    TERM_COL_LOSE,
    TERM_COL_INFO,
    TERM_COL_GENINFO,
    TERM_COL_HEADER_MINES,
    TERM_COL_NUM1, TERM_COL_NUM2, TERM_COL_NUM3, TERM_COL_NUM4,
    TERM_COL_NUM5, TERM_COL_NUM6, TERM_COL_NUM7, TERM_COL_NUM8
};

/* ------------------------------------------------------------------ */
/* STRUCTURES                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    int rows;
    int cols;
    int mines;
    int deterministic;   /* 1 = no-guess generation, 0 = classic random */
} Config;

typedef struct { 
    unsigned char flags; 
    unsigned char adj; 
} Cell;

typedef struct { 
    short r; 
    short c; 
} DirtyCell;

/* ------------------------------------------------------------------ */
/* GLOBAL ENGINE VARIABLES                                           */
/* ------------------------------------------------------------------ */
/* Core Game Engine States */
static Config cfg = { 16, 30, 99, 1 };
static Cell *board = NULL;

static int cursor_r = 0;
static int cursor_c = 0;
static int game_over = 0;       /* 0 = playing, 1 = win, -1 = lose */
static int mines_left = 0;
static int revealed_count = 0;
static int board_generated = 0; /* 0 until first Space click */

/* Dynamic Dirty-list Tracker System */
static DirtyCell *dirty_list = NULL;
static int dirty_cnt = 0;
static int dirty_cap = 0;

/* Platform Specific Storage & Context Variables */
#ifdef _WIN32
    static HANDLE hOut;
    #define COL_DEFAULT (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)

    static const WORD win_colors[9] = {
        0,
        FOREGROUND_BLUE  | FOREGROUND_INTENSITY, 
        FOREGROUND_GREEN | FOREGROUND_INTENSITY, 
        FOREGROUND_RED   | FOREGROUND_INTENSITY, 
        FOREGROUND_BLUE,                         
        FOREGROUND_RED,                          
        FOREGROUND_BLUE  | FOREGROUND_GREEN | FOREGROUND_INTENSITY, 
        FOREGROUND_BLUE  | FOREGROUND_GREEN | FOREGROUND_RED,       
        FOREGROUND_INTENSITY                     
    };
#else
    static struct termios orig_termios;

    static const char *linux_colors[9] = {
        "\033[0m",
        "\033[1;34m", 
        "\033[1;32m", 
        "\033[1;31m", 
        "\033[0;34m", 
        "\033[0;31m", 
        "\033[1;36m", 
        "\033[0;37m", 
        "\033[90m"    
    };
#endif

/* ------------------------------------------------------------------ */
/* FORWARD DECLARATIONS                                              */
/* ------------------------------------------------------------------ */
static void gotoxy(int x, int y);
static void cls(void);
static void term_set_color(int color, int is_cursor);
static void term_reset_color(void);
static void handle_input(void);
static void draw_cell(int r, int c);
static void draw_border(void);
static void draw_header(void);
static void draw_status(void);
static void full_redraw(void);
static void flush_dirty(void);

/* ------------------------------------------------------------------ */
/* CONFIG LOADER UTILITIES                                           */
/* ------------------------------------------------------------------ */
static void cfg_trim(char *s) {
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' ||
                     s[n-1] == '\r' || s[n-1] == '\n')) {
        s[--n] = '\0';
    }
}

static void load_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        cfg_trim(line);
        if (line[0] == '#' || line[0] == '\0') continue;
        char key[64] = {0}, val[64] = {0};
        if (sscanf(line, "%63[^=]=%63s", key, val) != 2) continue;
        cfg_trim(key); cfg_trim(val);
        if      (strcmp(key, "rows")          == 0) cfg.rows          = atoi(val);
        else if (strcmp(key, "cols")          == 0) cfg.cols          = atoi(val);
        else if (strcmp(key, "mines")         == 0) cfg.mines         = atoi(val);
        else if (strcmp(key, "deterministic") == 0) cfg.deterministic = atoi(val);
    }
    fclose(f);

    /* Safe dynamic boundaries clamping */
    if (cfg.rows < 2) cfg.rows = 2;
    if (cfg.cols < 2) cfg.cols = 2;
    int max_mines = cfg.rows * cfg.cols - 1;
    if (cfg.mines < 1)         cfg.mines = 1;
    if (cfg.mines > max_mines) cfg.mines = max_mines;
}

static void write_default_config(const char *path) {
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); return; }

    f = fopen(path, "w");
    if (!f) return;
    fprintf(f,
        "# Minesweeper configuration\n"
        "#\n"
        "# rows          - board height\n"
        "# cols          - board width\n"
        "# mines         - mine count\n"
        "# deterministic - no-guess mode (1=on / 0=off, default 1)\n"
        "#   When on, the board is guaranteed solvable without guessing.\n\n"
        "rows          = 16\n"
        "cols          = 30\n"
        "mines         = 99\n"
        "deterministic = 1\n"
    );
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* DIRTY-TRACKER ENGINE                                              */
/* ------------------------------------------------------------------ */
static void dirty_clear(void) { dirty_cnt = 0; }

static void dirty_add(int r, int c) {
    if (!dirty_list || dirty_cnt >= dirty_cap) return;
    dirty_list[dirty_cnt].r = (short)r;
    dirty_list[dirty_cnt].c = (short)c;
    dirty_cnt++;
}

static void dirty_header(void) { dirty_add(DIRTY_HEADER, 0); }
static void dirty_status(void) { dirty_add(DIRTY_STATUS, 0); }

/* ------------------------------------------------------------------ */
/* MATHEMATICAL CORE ENGINE & SOLVER                                 */
/* ------------------------------------------------------------------ */
static int calc_adj_kind(int r, int c, int kind) {
    int cnt = 0;
    for (int i = -1; i <= 1; i++) {
        for (int j = -1; j <= 1; j++) {
            int nr = r+i, nc = c+j;
            if (nr >= 0 && nr < cfg.rows && nc >= 0 && nc < cfg.cols)
                if (board[nr * cfg.cols + nc].flags & kind) cnt++;
        }
    }
    return cnt;
}

static void recalc_adj(void) {
    int r, c;
    for (r = 0; r < cfg.rows; r++) {
        for (c = 0; c < cfg.cols; c++) {
            if (board[r * cfg.cols + c].flags & CELL_MINE) continue;
            board[r * cfg.cols + c].adj = (unsigned char)calc_adj_kind(r, c, CELL_MINE);
        }
    }
}

static int solver_step(char *known) {
    int changed = 0, r, c, i, j;
    for (r = 0; r < cfg.rows; r++) {
        for (c = 0; c < cfg.cols; c++) {
            int idx = r * cfg.cols + c;
            if (known[idx] != 1) continue;
            int adj = board[idx].adj;
            int unk = 0, flagged = 0;
            
            for (i = -1; i <= 1; i++) {
                for (j = -1; j <= 1; j++) {
                    if (!i && !j) continue;
                    int nr = r+i, nc = c+j;
                    if (nr < 0 || nr >= cfg.rows || nc < 0 || nc >= cfg.cols) continue;
                    int ni = nr * cfg.cols + nc;
                    if      (known[ni] == 0) unk++;
                    else if (known[ni] == 2) flagged++;
                }
            }
            if (unk > 0 && unk == adj - flagged) {
                for (i = -1; i <= 1; i++) {
                    for (j = -1; j <= 1; j++) {
                        if (!i && !j) continue;
                        int nr = r+i, nc = c+j;
                        if (nr < 0 || nr >= cfg.rows || nc < 0 || nc >= cfg.cols) continue;
                        int ni = nr * cfg.cols + nc;
                        if (known[ni] == 0) { known[ni] = 2; changed++; }
                    }
                }
            }
            if (unk > 0 && adj == flagged) {
                for (i = -1; i <= 1; i++) {
                    for (j = -1; j <= 1; j++) {
                        if (!i && !j) continue;
                        int nr = r+i, nc = c+j;
                        if (nr < 0 || nr >= cfg.rows || nc < 0 || nc >= cfg.cols) continue;
                        int ni = nr * cfg.cols + nc;
                        if (known[ni] == 0) { known[ni] = 1; changed++; }
                    }
                }
            }
        }
    }
    return changed;
}

static void solver_open(char *known, int r, int c) {
    if (r < 0 || r >= cfg.rows || c < 0 || c >= cfg.cols) return;
    int idx = r * cfg.cols + c;
    if (known[idx] != 0) return;
    known[idx] = 1;
    if (board[idx].adj == 0) {
        int i, j;
        for (i = -1; i <= 1; i++)
            for (j = -1; j <= 1; j++)
                if (i || j) solver_open(known, r+i, c+j);
    }
}

static int is_solvable(int sr, int sc) {
    int total = cfg.rows * cfg.cols;
    char *known = (char*)calloc(total, 1);
    if (!known) return 0;

    solver_open(known, sr, sc);

    int progress = 1;
    while (progress) {
        progress = solver_step(known);
        int r, c;
        for (r = 0; r < cfg.rows; r++) {
            for (c = 0; c < cfg.cols; c++) {
                int idx = r * cfg.cols + c;
                if (known[idx] == 1 && board[idx].adj == 0)
                    solver_open(known, r, c);
            }
        }
    }

    int ok = 1;
    int r, c;
    for (r = 0; r < cfg.rows && ok; r++) {
        for (c = 0; c < cfg.cols && ok; c++) {
            int idx = r * cfg.cols + c;
            if (!(board[idx].flags & CELL_MINE) && known[idx] != 1)
                ok = 0;
        }
    }

    free(known);
    return ok;
}

/* ------------------------------------------------------------------ */
/* GAMEPLAY PROCEDURAL ENGINE                                        */
/* ------------------------------------------------------------------ */
static void place_mines_random(int safe_r, int safe_c) {
    int placed = 0;
    while (placed < cfg.mines) {
        int r = rand() % cfg.rows;
        int c = rand() % cfg.cols;
        if (abs(r - safe_r) <= 1 && abs(c - safe_c) <= 1) continue;
        int idx = r * cfg.cols + c;
        if (board[idx].flags & CELL_MINE) continue;
        board[idx].flags |= CELL_MINE;
        placed++;
    }
}

static void init_board(int first_r, int first_c) {
    size_t total_cells = cfg.rows * cfg.cols;
    memset(board, 0, total_cells * sizeof(Cell));
    mines_left     = cfg.mines;
    revealed_count = 0;
    game_over      = 0;

    if (cfg.deterministic) {
        int attempts = 0;
        do {
            memset(board, 0, total_cells * sizeof(Cell));
            place_mines_random(first_r, first_c);
            recalc_adj();
            attempts++;
            if (attempts > 2000) break;
        } while (!is_solvable(first_r, first_c));
    } else {
        place_mines_random(first_r, first_c);
        recalc_adj();
    }
}

static void reveal_cell(int r, int c) {
    if (r < 0 || r >= cfg.rows || c < 0 || c >= cfg.cols) return;
    Cell *cell = &board[r * cfg.cols + c];
    if (cell->flags & CELL_REVEALED) {
        if (calc_adj_kind(r, c, CELL_FLAGGED) != cell->adj) return;
        for (int i = -1; i <= 1; i++) {
            for (int j = -1; j <= 1; j++) {
                int nr = r+i, nc = c+j;
                if (nr >= 0 && nr < cfg.rows && nc >= 0 && nc < cfg.cols) {
                    if (board[nr * cfg.cols + nc].flags & CELL_REVEALED ||
                        board[nr * cfg.cols + nc].flags & CELL_FLAGGED) continue;
                    reveal_cell(nr, nc);
                }
            }
        }
        return;
    }
    if (cell->flags & CELL_FLAGGED)  return;
    cell->flags |= CELL_REVEALED;
    revealed_count++;
    dirty_add(r, c);
    if (cell->adj == 0 && !(cell->flags & CELL_MINE)) {
        int i, j;
        for (i = -1; i <= 1; i++)
            for (j = -1; j <= 1; j++)
                if (i || j) reveal_cell(r+i, c+j);
    }
}

static void reveal_all_mines(void) {
    int r, c;
    for (r = 0; r < cfg.rows; r++) {
        for (c = 0; c < cfg.cols; c++) {
            int idx = r * cfg.cols + c;
            if ((board[idx].flags & CELL_MINE) && !(board[idx].flags & CELL_REVEALED)) {
                board[idx].flags |= CELL_REVEALED;
                dirty_add(r, c);
            }
        }
    }
}

static void game_open_cell(int r, int c) {
    if (game_over) return;
    Cell *cell = &board[r * cfg.cols + c];
    if (cell->flags & CELL_FLAGGED)  return;

    if (!board_generated) {
        init_board(r, c);
        board_generated = 1;
    }

    if (cell->flags & CELL_MINE) {
        cell->flags |= CELL_REVEALED;
        dirty_add(r, c);
        game_over = -1;
        reveal_all_mines();
        dirty_status();
        return;
    }
    reveal_cell(r, c);
    if (revealed_count == cfg.rows * cfg.cols - cfg.mines)
        game_over = 1;
    dirty_status();
}

static void game_toggle_flag(int r, int c) {
    if (game_over) return;
    Cell *cell = &board[r * cfg.cols + c];
    if (cell->flags & CELL_REVEALED) return;
    if (cell->flags & CELL_FLAGGED) {
        cell->flags &= ~CELL_FLAGGED;
        mines_left++;
    } else {
        cell->flags |= CELL_FLAGGED;
        mines_left--;
    }
    dirty_add(r, c);
    dirty_header();
    dirty_status();
}

static void game_reset(void) {
    cursor_r = cursor_c = 0;
    board_generated = 0;
    if (board) {
        memset(board, 0, cfg.rows * cfg.cols * sizeof(Cell));
    }
    mines_left     = cfg.mines;
    revealed_count = 0;
    game_over      = 0;
}

/* ------------------------------------------------------------------ */
/* INTERACTIVE CORE UI DESIGN ENGINE                                 */
/* ------------------------------------------------------------------ */
static void draw_cell(int r, int c) {
    Cell *cell = &board[r * cfg.cols + c];
    int sx = BOARD_X + c * 2;
    int sy = BOARD_Y + r;
    int is_cur = (r == cursor_r && c == cursor_c);

    gotoxy(sx, sy);

    if (cell->flags & CELL_REVEALED) {
        if (cell->flags & CELL_MINE) {
            term_set_color(TERM_COL_MINE, is_cur);
            printf("%s ", CH_MINE);
        } else if (cell->adj > 0) {
            term_set_color(TERM_COL_NUM1 + (cell->adj - 1), is_cur);
            printf("%d ", cell->adj);
        } else {
            term_set_color(TERM_COL_DEFAULT, is_cur);
            printf("%s ", CH_EMPTY);
        }
    } else if (cell->flags & CELL_FLAGGED) {
        term_set_color(TERM_COL_FLAG, is_cur);
        printf("%s ", CH_FLAG);
    } else {
        term_set_color(TERM_COL_HIDDEN, is_cur);
        printf("  ");
    }
    term_reset_color();
}

static void draw_border(void) {
    int r, c;
    term_set_color(TERM_COL_BORDER, 0);
    gotoxy(BOARD_X-1, BOARD_Y-1);
    printf("%s", CH_TL);
    for (c = 0; c < cfg.cols*2; c++) printf("%s", CH_HBAR);
    printf("%s", CH_TR);
    for (r = 0; r < cfg.rows; r++) {
        gotoxy(BOARD_X-1,         BOARD_Y+r); printf("%s", CH_VBAR);
        gotoxy(BOARD_X+cfg.cols*2, BOARD_Y+r); printf("%s", CH_VBAR);
    }
    gotoxy(BOARD_X-1, BOARD_Y+cfg.rows);
    printf("%s", CH_BL);
    for (c = 0; c < cfg.cols*2; c++) printf("%s", CH_HBAR);
    printf("%s", CH_BR);
    term_reset_color();
}

static void draw_header(void) {
    gotoxy(BOARD_X, 0);
    term_set_color(TERM_COL_TITLE, 0);
    printf("*** MINESWEEPER ***");

    if (cfg.deterministic) {
        gotoxy(BOARD_X + 22, 0);
        term_set_color(TERM_COL_GENINFO, 0);
        printf("[Deterministic mode - no guessing required]");
    }

    gotoxy(BOARD_X, 1);
    term_set_color(TERM_COL_HEADER_MINES, 0);
    printf("Mines: %3d", mines_left);

    gotoxy(BOARD_X + 14, 1);
    term_set_color(TERM_COL_INFO, 0);
    printf("[Arrows] Move  [Space] Open  [F] Flag  [R] Restart  [Q] Quit");
    term_reset_color();
}

static void draw_status(void) {
    int y = BOARD_Y + cfg.rows + 1;
    gotoxy(BOARD_X, y);
    int w = cfg.cols * 2 + 4, i;
    for (i = 0; i < w; i++) putchar(' ');
    gotoxy(BOARD_X, y);

    if (game_over == 1) {
        term_set_color(TERM_COL_WIN, 0);
        printf("  *** YOU WIN! Congratulations! *** [R] Restart");
    } else if (game_over == -1) {
        term_set_color(TERM_COL_LOSE, 0);
        printf("  *** BOOM! You hit a mine!      *** [R] Restart");
    } else if (!board_generated) {
        term_set_color(TERM_COL_INFO, 0);
        printf("  [Space] to place first click and generate board");
    } else {
        term_set_color(TERM_COL_DEFAULT, 0);
        printf("  [%2d,%2d]  Revealed: %d / %d",
               cursor_r+1, cursor_c+1,
               revealed_count, cfg.rows*cfg.cols - cfg.mines);
    }
    term_reset_color();
}

static void full_redraw(void) {
    cls();
    draw_border();
    draw_header();
    int r, c;
    for (r = 0; r < cfg.rows; r++) {
        for (c = 0; c < cfg.cols; c++) {
            draw_cell(r, c);
        }
    }
    draw_status();
#ifndef _WIN32
    fflush(stdout);
#endif
}

static void flush_dirty(void) {
    int i;
    int need_header = 0, need_status = 0;
    for (i = 0; i < dirty_cnt; i++) {
        int r = dirty_list[i].r;
        int c = dirty_list[i].c;
        if      (r == DIRTY_HEADER) need_header = 1;
        else if (r == DIRTY_STATUS) need_status = 1;
        else    draw_cell(r, c);
    }
    if (need_header) draw_header();
    if (need_status) draw_status();
    dirty_clear();
#ifndef _WIN32
    fflush(stdout);
#endif
}

/* ------------------------------------------------------------------ */
/* PLATFORM DRIVERS (LOW-LEVEL OPERATING SYSTEM HOOKS)               */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
static void gotoxy(int x, int y) {
    COORD c = { (SHORT)x, (SHORT)y };
    SetConsoleCursorPosition(hOut, c);
}

static void cls(void) {
    COORD origin = {0,0}; DWORD written;
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hOut, &csbi);
    DWORD size = csbi.dwSize.X * csbi.dwSize.Y;
    FillConsoleOutputCharacter(hOut, ' ', size, origin, &written);
    FillConsoleOutputAttribute(hOut, COL_DEFAULT, size, origin, &written);
    gotoxy(0,0);
}

static void term_set_color(int color, int is_cursor) {
    if (is_cursor) {
        WORD cursor_attr = BACKGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY;
        SetConsoleTextAttribute(hOut, cursor_attr);
        return;
    }

    WORD attr = 0, bg = 0;
    if (color == TERM_COL_HIDDEN) {
        bg = BACKGROUND_INTENSITY;
    } else if (color == TERM_COL_MINE || color == TERM_COL_FLAG) {
        bg = BACKGROUND_RED;
    }

    if (color >= TERM_COL_NUM1 && color <= TERM_COL_NUM8) {
        attr = win_colors[color - TERM_COL_NUM1 + 1] | bg;
    } else {
        switch (color) {
            case TERM_COL_BORDER:  attr = (FOREGROUND_GREEN | FOREGROUND_INTENSITY) | bg; break;
            case TERM_COL_TITLE:   attr = (FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY) | bg; break;
            case TERM_COL_FLAG:    attr = (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY) | bg; break;
            case TERM_COL_MINE:    attr = (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY) | bg; break;
            case TERM_COL_HIDDEN:  attr = (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED) | bg; break;
            case TERM_COL_WIN:     attr = (FOREGROUND_GREEN | FOREGROUND_INTENSITY) | bg; break;
            case TERM_COL_LOSE:    attr = (FOREGROUND_RED | FOREGROUND_INTENSITY) | bg; break;
            case TERM_COL_INFO:    attr = (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY) | bg; break;
            case TERM_COL_GENINFO: attr = (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY) | bg; break;
            case TERM_COL_HEADER_MINES: attr = (FOREGROUND_RED | FOREGROUND_INTENSITY) | bg; break;
            default:               attr = (FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED) | bg; break;
        }
    }
    SetConsoleTextAttribute(hOut, attr);
}

static void term_reset_color(void) {
    SetConsoleTextAttribute(hOut, COL_DEFAULT);
}

static void hide_cursor(void) {
    CONSOLE_CURSOR_INFO ci = {1, FALSE};
    SetConsoleCursorInfo(hOut, &ci);
}

static void handle_input(void) {
    int ch = _getch();
    if (ch == 0 || ch == 0xE0) {
        int ext = _getch();
        int pr = cursor_r, pc = cursor_c;
        switch (ext) {
            case 72: cursor_r = (cursor_r - 1 + cfg.rows) % cfg.rows; break; /* Up */
            case 80: cursor_r = (cursor_r + 1) % cfg.rows;            break; /* Down */
            case 75: cursor_c = (cursor_c - 1 + cfg.cols) % cfg.cols; break; /* Left */
            case 77: cursor_c = (cursor_c + 1) % cfg.cols;            break; /* Right */
            default: return;
        }
        dirty_add(pr, pc);
        dirty_add(cursor_r, cursor_c);
        dirty_status();
        flush_dirty();
        return;
    }

    switch (ch) {
        case ' ':           game_open_cell(cursor_r, cursor_c);   flush_dirty(); break;
        case 'f': case 'F': game_toggle_flag(cursor_r, cursor_c); flush_dirty(); break;
        case 'r': case 'R': game_reset(); full_redraw(); break;
        case 'q': case 'Q': case 27:
            cls(); gotoxy(0,0); term_reset_color();
            printf("Thanks for playing!\n");
            free(board);
            free(dirty_list);
            exit(0);
    }
}

#else
static void term_restore(void) {
    tcsetattr(STDIN_FILENO, TCSANOW, &orig_termios);
    printf("\033[?25h\033[0m");
    fflush(stdout);
}

static void term_raw(void) {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(term_restore);
    struct termios raw = orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~(IXON | ICRNL);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void gotoxy(int x, int y) {
    printf("\033[%d;%dH", y+1, x+1);
}

static void cls(void) {
    printf("\033[2J\033[H");
}

static void term_set_color(int color, int is_cursor) {
    printf("\033[0m"); 
    if (is_cursor) {
        printf("\033[1;37m\033[42m");
        return;
    }

    if (color >= TERM_COL_NUM1 && color <= TERM_COL_NUM8) {
        printf("%s", linux_colors[color - TERM_COL_NUM1 + 1]);
    } else {
        switch (color) {
            case TERM_COL_BORDER:  printf("\033[1;32m"); break; 
            case TERM_COL_TITLE:   printf("\033[1;33m"); break; 
            case TERM_COL_FLAG:    printf("\033[1;37m"); break; 
            case TERM_COL_MINE:    printf("\033[1;37m"); break; 
            case TERM_COL_HIDDEN:  break;
            case TERM_COL_WIN:     printf("\033[1;32m"); break;
            case TERM_COL_LOSE:    printf("\033[1;31m"); break;
            case TERM_COL_INFO:    printf("\033[1;36m"); break; 
            case TERM_COL_GENINFO: printf("\033[1;33m"); break;
            case TERM_COL_HEADER_MINES: printf("\033[1;31m"); break; 
            default:               printf("\033[37m");   break; 
        }
    }

    if (color == TERM_COL_HIDDEN) {
        printf("\033[100m"); 
    } else if (color == TERM_COL_MINE || color == TERM_COL_FLAG) {
        printf("\033[41m"); 
    }
}

static void term_reset_color(void) {
    printf("\033[0m");
}

static int read_byte(void) {
    unsigned char ch;
    if (read(STDIN_FILENO, &ch, 1) != 1) return -1;
    return ch;
}

static void handle_input(void) {
    int ch = read_byte();
    if (ch < 0) return;

    if (ch == 0x1B) {
        struct timeval tv = {0, 50000};
        fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO+1, &fds, NULL, NULL, &tv) <= 0) {
            cls(); gotoxy(0,0);
            printf("Thanks for playing!\n");
            free(board);
            free(dirty_list);
            exit(0);
        }
        int c2 = read_byte();
        if (c2 == '[') {
            int c3 = read_byte();
            int pr = cursor_r, pc = cursor_c;
            switch (c3) {
                case 'A': cursor_r = (cursor_r - 1 + cfg.rows) % cfg.rows; break; /* Up */
                case 'B': cursor_r = (cursor_r + 1) % cfg.rows;            break; /* Down */
                case 'D': cursor_c = (cursor_c - 1 + cfg.cols) % cfg.cols; break; /* Left */
                case 'C': cursor_c = (cursor_c + 1) % cfg.cols;            break; /* Right */
                default: return;
            }
            dirty_add(pr, pc);
            dirty_add(cursor_r, cursor_c);
            dirty_status();
            flush_dirty();
        }
        return;
    }

    switch (ch) {
        case ' ':           game_open_cell(cursor_r, cursor_c);   flush_dirty(); break;
        case 'f': case 'F': game_toggle_flag(cursor_r, cursor_c); flush_dirty(); break;
        case 'r': case 'R': game_reset(); full_redraw(); break;
        case 'q': case 'Q':
            cls(); gotoxy(0,0);
            printf("Thanks for playing!\n");
            free(board);
            free(dirty_list);
            exit(0);
    }
}
#endif

/* ------------------------------------------------------------------ */
/* MAIN ROUTINE (DYNAMIC ENVIRONMENT ALLOCATION)                     */
/* ------------------------------------------------------------------ */
int main(int argc, char **argv) {
    srand((unsigned int)time(NULL));
    char cfg_path[512];

#ifdef _WIN32
    if (argc >= 2) {
        strncpy(cfg_path, argv[1], sizeof(cfg_path)-1);
    } else {
        GetModuleFileNameA(NULL, cfg_path, sizeof(cfg_path));
        char *slash = strrchr(cfg_path, '\\');
        if (!slash) slash = strrchr(cfg_path, '/');
        if (slash) *(slash+1) = '\0';
        strncat(cfg_path, "minesweeper.cfg", sizeof(cfg_path)-1-strlen(cfg_path));
    }
    write_default_config(cfg_path);
    load_config(cfg_path);

    /* Allocate memory mappings dynamically according to loaded configuration */
    board = (Cell *)calloc(cfg.rows * cfg.cols, sizeof(Cell));
    dirty_cap = cfg.rows * cfg.cols + 32;
    dirty_list = (DirtyCell *)malloc(dirty_cap * sizeof(DirtyCell));
    if (!board || !dirty_list) {
        fprintf(stderr, "Allocation failed: Out of Memory\n");
        return 1;
    }

    hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTitle("Minesweeper");
    SetConsoleOutputCP(866);

    COORD buf = { (SHORT)(BOARD_X + cfg.cols*2 + 4), (SHORT)(BOARD_Y + cfg.rows + 6) };
    SetConsoleScreenBufferSize(hOut, buf);
    SMALL_RECT win = { 0, 0, (SHORT)(buf.X-1), (SHORT)(buf.Y-1) };
    SetConsoleWindowInfo(hOut, TRUE, &win);

    hide_cursor();
    game_reset();
    full_redraw();

    while (1) handle_input();
    return 0;

#else
    if (argc >= 2) {
        strncpy(cfg_path, argv[1], sizeof(cfg_path)-1);
    } else {
        char self[PATH_MAX] = {0};
        ssize_t n = readlink("/proc/self/exe", self, sizeof(self)-1);
        if (n > 0) {
            self[n] = '\0';
            strncpy(cfg_path, dirname(self), sizeof(cfg_path)-1);
        } else {
            strncpy(cfg_path, ".", sizeof(cfg_path)-1);
        }
        strncat(cfg_path, "/minesweeper.cfg", sizeof(cfg_path)-1-strlen(cfg_path));
    }
    write_default_config(cfg_path);
    load_config(cfg_path);

    /* Allocate memory mappings dynamically according to loaded configuration */
    board = (Cell *)calloc(cfg.rows * cfg.cols, sizeof(Cell));
    dirty_cap = cfg.rows * cfg.cols + 32;
    dirty_list = (DirtyCell *)malloc(dirty_cap * sizeof(DirtyCell));
    if (!board || !dirty_list) {
        fprintf(stderr, "Allocation failed: Out of Memory\n");
        return 1;
    }

    term_raw();
    printf("\033[?25l");

    game_reset();
    full_redraw();

    while (1) handle_input();
    return 0;
#endif
}
