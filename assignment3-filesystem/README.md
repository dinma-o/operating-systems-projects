[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/zegDotdy)
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
# Name : Chidinma Obi-Okoye
# SID : 1756548
# CCID : obiokoye
# - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

# CMPUT 379 - Assignment 3: UNIX File System Simulator

## Overview
This program implements a UNIX-like file system simulator that mounts a 128 KB virtual disk. It handles file creation, deletion, reading, writing, and directory management using a contiguous block allocation strategy. It also supports consistency checking upon mounting and disk defragmentation.

## Design Choices

### Global Variables
I decided to use global variables for the `Superblock`, the data `buffer`, and the `current_dir_inode`. Since almost every function in the program needs access to the file system state (to check bitmaps, read inodes, or write data), passing these pointers into every single helper function would have made the function signatures very messy. Using globals made the code cleaner and easier to manage.

### Bit Manipulation Helpers
To handle the packed bit fields (where one byte stores two values, like size and status), I wrote helper functions like `is_inode_used`, `get_file_size`, and `set_parent_index`. This abstracts the bitwise AND/OR operations away from the main logic, reducing the chance of bugs when reading/writing metadata.

### Consistency Checking
The assignment requires checking for 6 specific errors in a strict order. I implemented `check_consistency` as a single function that runs through the checks sequentially (1 to 6). It returns the error code immediately upon finding the first inconsistency. This ensures that I always report the highest-priority error first, as required by the spec.

### Handling 5-Character Names
The spec mentions that filenames can be exactly 5 characters long without a null terminator on disk. To handle this safely in C, I created a helper `get_inode_name` that always copies the name into a local 6-byte buffer and manually adds a `\0` at the end. This prevents buffer overflows when using `printf` or `strcmp`.

### Defragmentation Strategy
For the `fs_defrag` function, I used a "sort and compact" approach. I first collect all file inodes and sort them based on their *current* start block. I then iterate through this sorted list and move each file to the earliest available free block (starting right after the superblock). After moving all files, I zero out the rest of the disk and rebuild the free-space bitmap from scratch. This prevents data loss during the move.

## System Calls Used

* `open` / `close`: Used to access the virtual disk file.
* `read` / `write`: Used to load the superblock and transfer data blocks to/from the disk.
* `lseek`: Used to jump to specific block offsets (block_num * 1024) within the disk file.
* `sscanf`: Used for parsing command input arguments.
* `memset` / `memcpy`: Used for buffer management and initializing inodes.

## Testing Strategy

I tested the program using the provided `create_fs` utility and a combination of automated and manual tests.

1.  **Functional Testing:** I used the provided `test.py` script to verify basic operations. I also created custom input files to test specific edge cases, such as:
    * Creating files with names exactly 5 characters long.
    * Attempting to create files larger than the available free space.
    * Recursively deleting directories containing multiple files.
    * Navigating directories using `..` from the root (to ensure it stays at root).

2.  **Consistency Checks:** Since the provided `create_fs` tool only creates valid disks, I wrote a Python script to deliberately corrupt specific bytes in the disk file. I generated 6 different corrupted disks (one for each error code) and verified that `fs_mount` correctly identified the error codes in the specified order.

3.  **Memory Testing:** I ran the program using Valgrind to ensure there were no memory leaks or invalid accesses:
    ```bash
    valgrind --leak-check=yes ./fs input_file
    ```

    ### Testing Commands
```bash
# Compile
make clean && make

# Run single test
./fs tests/test1/input > stdout.txt 2> stderr.txt
diff stdout.txt tests/test1/stdout_expected
diff stderr.txt tests/test1/stderr_expected

# Run all tests
python3 test.py

# Memory leak check
valgrind --leak-check=full --show-leak-kinds=all ./fs tests/test1/input
```
## Sources

* Assignment 3 Specification (a3_cmput379.pdf)
* Linux Man Pages (man 2 open, man 2 lseek, man 3 strcasecmp)
