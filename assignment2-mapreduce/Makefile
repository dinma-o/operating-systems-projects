# Makefile for MapReduce Library and Word Count Application
# CMPUT 379 Assignment 2

# Compiler and flags
CC = gcc
CFLAGS = -Wall -pthread -g

# Target: Build thread pool object file
threadpool.o: threadpool.c threadpool.h
	$(CC) $(CFLAGS) -c threadpool.c

# Target: Build mapreduce object file
mapreduce.o: mapreduce.c mapreduce.h threadpool.h
	$(CC) $(CFLAGS) -c mapreduce.c

# Target: Build word count application object file
distwc.o: distwc.c mapreduce.h
	$(CC) $(CFLAGS) -c distwc.c

# Target: Link all object files to create executable
wordcount: threadpool.o mapreduce.o distwc.o
	$(CC) $(CFLAGS) -o wordcount threadpool.o mapreduce.o distwc.o

# Target: Build all (default)
all: wordcount

# Target: Clean build artifacts
clean:
	rm -f *.o wordcount result-*.txt

# Target: Run valgrind memory check
valgrind: wordcount
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./wordcount sample1.txt sample2.txt sample3.txt

# Target: Run helgrind race condition check
helgrind: wordcount
	valgrind --tool=helgrind ./wordcount sample1.txt sample2.txt sample3.txt

# Target: Run test with all sample files
test: wordcount
	./wordcount sample*.txt

.PHONY: all clean valgrind helgrind test