# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -g

# Target executable
TARGET = dragonshell

# Default target
all: $(TARGET)

# How to build the executable
$(TARGET): dragonshell.o
	$(CC) $(CFLAGS) -o $(TARGET) dragonshell.o

# How to build the object file
dragonshell.o: dragonshell.c
	$(CC) $(CFLAGS) -c dragonshell.c

# Convenience targets
compile: $(TARGET)

clean:
	rm -f *.o $(TARGET)
