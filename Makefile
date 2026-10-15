# Сборка чата: make        (Linux, macOS, MSYS2/MinGW под Windows)
#              make clean

CC     := gcc
CFLAGS := -std=c99 -Wall -Wextra -pedantic -O2

ifeq ($(OS),Windows_NT)
    EXE  := .exe
    LIBS := -lws2_32
else
    EXE  :=
    LIBS := -pthread
    CFLAGS += -pthread
endif

all: server$(EXE) client$(EXE)

server$(EXE): server.c common.c common.h
	$(CC) $(CFLAGS) -o $@ server.c common.c $(LIBS)

client$(EXE): client.c common.c common.h
	$(CC) $(CFLAGS) -o $@ client.c common.c $(LIBS)

clean:
	rm -f server$(EXE) client$(EXE)

.PHONY: all clean
