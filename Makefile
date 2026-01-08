# CMPUT 379 - Assignment 3 Makefile
# Name: Chidinma Obi-Okoye
# CCID: obiokoye

CC = gcc
CFLAGS = -Wall -Werror -g -std=c11
TARGET = fs
OBJS = fs-sim.o

# Default target
fs: $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

# Compile target
compile: $(OBJS)

#Object file generation
fs-sim.o: fs-sim.c fs-sim.h
	$(CC) $(CFLAGS) -c fs-sim.c

# Clean target
clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: clean compile