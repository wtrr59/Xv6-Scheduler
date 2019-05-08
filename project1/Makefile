.SUFFIXES: .c .o

CC=gcc -g -Wall -I./include/

INC = ./include/
BIN = ./bin/
SRCS:=$(wildcard src/*.c)
OBJS:=$(SRCS:.c=.o)
LIB = ./lib/

TARGET=shell

$(TARGET): $(OBJS)
	$(CC) -o $(BIN)$(TARGET) $(OBJS) -L$(LIB) -lpthread

clean:
	rm $(BIN)$(TARGET) $(OBJS)
