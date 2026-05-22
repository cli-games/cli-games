TARGET = minesweeper

CC = gcc
CFLAGS = -Wall -Wextra -O2

ifeq ($(OS),Windows_NT)
    EXE = .exe
    RM = del /Q /F
    CLEAN_TARGET = $(TARGET)$(EXE)
else
    EXE =
    RM = rm -f
    CLEAN_TARGET = $(TARGET)
endif

SRCS = minesweeper.c
OBJS = $(SRCS:.c=.o)

$(TARGET)$(EXE): $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) -o $(TARGET)$(EXE)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

all: $(TARGET)$(EXE)

clean:
	$(RM) $(OBJS) $(CLEAN_TARGET)

.PHONY: all clean
