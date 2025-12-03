/**
 * CMPUT 379 Assignment 3 - Filesystem Simulator    
 * Name: Chidinma Obi-Okoye
 * CCID: obiokoye
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>  // For strcasecmp
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include "fs-sim.h"

//Global variables
static Superblock sb;
static uint8_t buffer[1024];
static int current_dir_inode = 127; 
static int disk_fd = -1;
static int is_mounted = 0;
static char current_disk_name[256];

/* ========================================================================
 * BIT MANIPULATION & INODE HELPERS
 * ======================================================================== */

/**
 * Checks if an inode is marked as "in use".
 * It returns 1 if used, 0 if free. 
 * Bit 7 of isused_size indicates usage. 
 * Mask is used to isolate this bit.
 */
static inline int is_inode_used(const Inode *inode) {
    return (inode->isused_size & 0x80) != 0;
}

/**
 * Sets or clears the "used" flag of an inode.
 * If used is 1, marks as used; if 0, marks as free.
 */
static inline void set_inode_used(Inode *inode, int used) {
    if (used) {
        inode->isused_size |= 0x80;   // Set bit 7
    } else {
        inode->isused_size &= 0x7F;   // Clear bit 7
    }
}

/**
 * Extracts file size (in blocks) from inode.
 * Mask is used to get bits 0-6 of isused_size due to bit packing.
 */
static inline int get_file_size(const Inode *inode) {
    return inode->isused_size & 0x7F;
}

/**
 * Sets file size (in blocks) in inode.
 */
static inline void set_file_size(Inode *inode, int size) {
    inode->isused_size = (inode->isused_size & 0x80) | (size & 0x7F);
}

/**
 * Checks if an inode represents a directory by checking bit 7 of isdir_parent field.
 * @return: 1 if directory, 0 if file
 */
static inline int is_directory(const Inode *inode) {
    return (inode->isdir_parent & 0x80) != 0;
}

/**
 * Sets or clears the "directory" flag of an inode.
 */
static inline void set_is_directory(Inode *inode, int is_dir) {
    if (is_dir) {
        inode->isdir_parent |= 0x80;   // Set bit 7
    } else {
        inode->isdir_parent &= 0x7F;   // Clear bit 7
    }
}

/**
 * Gets parent inode index from inode.
 */
static inline int get_parent_index(const Inode *inode) {
    return inode->isdir_parent & 0x7F;
}

/**
 * Sets parent inode index in inode.
 */
static inline void set_parent_index(Inode *inode, int parent) {
    inode->isdir_parent = (inode->isdir_parent & 0x80) | (parent & 0x7F);
}

/* ========================================================================
 * FREE BLOCK LIST HELPERS
 * These functions manage the free-space bitmap in the superblock.
 * ======================================================================== */

/**
 * Checks if a block is marked free in the free-space list.
 */
static int is_block_free(int block_num) {
    int byte_idx = block_num / 8; //Finds byte index
    int bit_idx = 7 - (block_num % 8); //Finds bit index within byte (7 is MSB)
    return (sb.free_block_list[byte_idx] & (1 << bit_idx)) == 0; //Returns 1 if free (bit=0), 0 if used (bit=1)
}

/**
 * Marks a block as free or used in the free-space list.
 */
static void set_block_free(int block_num, int free) {
    int byte_idx = block_num / 8; //Finds byte index
    int bit_idx = 7 - (block_num % 8); //Finds bit index within byte (7 is MSB)
    
    if (free) {
        sb.free_block_list[byte_idx] &= ~(1 << bit_idx);   // Clear bit (free)   
         } else {
         sb.free_block_list[byte_idx] |= (1 << bit_idx);  // Set bit (used)
    }
}

/**
 * Marks a contiguous range of blocks as used or free.
 * start: Starting block index
 * count: Number of blocks
 * used: 1 to mark used, 0 to mark free
 */
static void mark_blocks_used(int start, int count, int used) {
    for (int i = 0; i < count; i++) {
        set_block_free(start + i, !used);
    }
}

/**
 * Finds the first contiguous sequence of free blocks.
 * size: Number of contiguous blocks needed
 * return: Starting block index, or -1 if not found
 */
static int find_contiguous_blocks(int size) {
    // Edge case: requesting 0 blocks
    if (size == 0) return -1; // Invalid request

    // Scan for first contiguous run, skipping block 0 (superblock)
    for (int start = 1; start <= 128 - size; start++) {
        int all_free = 1;
        for (int i = 0; i < size; i++) {
            if (!is_block_free(start + i)) {
                all_free = 0;
                break;
            }
        }
        if (all_free) {
            return start;
        }
    }
    
    return -1;  // No contiguous space found
}

/* ========================================================================
 * INODE NAME HELPERS
 * File names can be exactly 5 characters with NO null terminator
 * ======================================================================== */

/**
 * Safely extracts name from inode into null-terminated buffer.
 * - Copy up to 5 characters
 * - Always null-terminate at position 5
 */
static void get_inode_name(const Inode *inode, char *dest) {
    memcpy(dest, inode->name, 5);
    dest[5] = '\0';  // Force null termination
}

/**
 * Stores name into inode (handles 5 maximum)
 */
static void set_inode_name(Inode *inode, const char *name) {
    memset(inode->name, 0, 5); //sets first 5 bytes to 0
    int len = strlen(name);
    if (len > 5) len = 5; // Limit to 5 characters
    memcpy(inode->name, name, len); // Copy name into inode
}

/**
 * Case-insensitive comparison of inode name with target string.
 */
static int inode_name_equals(const Inode *inode, const char *name) {
    char inode_name[6]; // Include space for null terminator
    get_inode_name(inode, inode_name);
    return strcasecmp(inode_name, name) == 0; // Returns 1 if equal, 0 otherwise
}

/* ========================================================================
 * DISK I/O HELPERS
 * ======================================================================== */

/**
 * Writes in-memory superblock back to disk
 * Called after any modification to filesystem metadata
 */
static void save_superblock(void) {
    lseek(disk_fd, 0, SEEK_SET); // Move to start of disk
    write(disk_fd, &sb, sizeof(Superblock)); // Write superblock
}

/**
 * Reads a data block from disk into buffer. 
 * Must be 1024 bytes
 */
static void read_block(int block_num, uint8_t *data) {
    lseek(disk_fd, block_num * 1024, SEEK_SET);
    read(disk_fd, data, 1024);
}

/**
 * Writes a data block from buffer to disk
 */
static void write_block(int block_num, const uint8_t *data) {
    lseek(disk_fd, block_num * 1024, SEEK_SET);
    write(disk_fd, data, 1024);
}

/* ========================================================================
 * INODE SEARCH HELPERS
 * ======================================================================== */

/**
 * Finds first unused inode in superblock.
 */
static int find_free_inode(void) {
    for (int i = 0; i < 126; i++) {
        if (!is_inode_used(&sb.inode[i])) {
            return i;
        }
    }
    return -1;
}

/**
 * Searches for file/directory by name in a specific directory.
 */
static int find_inode_by_name(const char *name, int parent_inode) {
    for (int i = 0; i < 126; i++) {
        if (is_inode_used(&sb.inode[i]) &&
            get_parent_index(&sb.inode[i]) == parent_inode && // Check parent
            inode_name_equals(&sb.inode[i], name)) { // Check name
            return i;
        }
    }
    return -1;
}

/**
 * Counts number of files/directories in a directory.
 *  This count is used for fs_ls output
 */
static int count_children(int dir_inode) {
    int count = 0;
    for (int i = 0; i < 126; i++) {
        if (is_inode_used(&sb.inode[i]) &&
            get_parent_index(&sb.inode[i]) == dir_inode) {
            count++;
        }
    }
    // Add 2 for . and ..
    return count + 2;
}

/* ========================================================================
 * CONSISTENCY CHECKING
 * Returns the first error code detected ONLY
 * ======================================================================== */

/**
 * Helper to check block status from a specific unmounted superblock
 */
static int is_block_free_from_sb(const Superblock *temp_sb, int block_num) {
    int byte_idx = block_num / 8;
    int bit_idx = 7 - (block_num % 8);
    return (temp_sb->free_block_list[byte_idx] & (1 << bit_idx)) == 0;
}

/**
 * Performs 6 consistency checks on a superblock.
 * Returns 0 if all checks pass, or error code (1-6) of first failure.
 * 
 * CHECK ORDER :
 * 1. Free inodes must be all-zero; used inodes must have nonzero name
 * 2. File start_block and last block must be in range [1, 127]
 * 3. Directory size and start_block must be zero
 * 4. Parent index validation (not self, not 126, parent must be dir)
 * 5. Names must be unique within each directory
 * 6. Block allocation matches free-space list
 */
static int check_consistency(const Superblock *temp_sb) {
    /* ====================================================================
     * CHECK 1: Free inode validation
     * - If inode is free (bit 7 of isused_size = 0), all 8 bytes must be 0
     * - If inode is used (bit 7 of isused_size = 1), name[0] must be nonzero
     * ==================================================================== */
    for (int i = 0; i < 126; i++) {
        int used = (temp_sb->inode[i].isused_size & 0x80) != 0;
        
        if (!used) {
            // Free inode: all bytes must be zero
            const uint8_t *bytes = (const uint8_t*)&temp_sb->inode[i];
            for (int j = 0; j < 8; j++) {
                if (bytes[j] != 0) {
                    return 1;  // Free inode has non-zero data
                }
            }
        } else {
            // Used inode: name must start with nonzero byte
            if (temp_sb->inode[i].name[0] == 0) {
                return 1;  // Used inode has zero name
            }
        }
    }
    
    /* ====================================================================
     * CHECK 2: File block range validation
     * - For files, start_block must be in [1, 127]
     * - Last block (start_block + size - 1) must also be in [1, 127]
     * ==================================================================== */
    for (int i = 0; i < 126; i++) {
        int used = (temp_sb->inode[i].isused_size & 0x80) != 0;
        int is_dir = (temp_sb->inode[i].isdir_parent & 0x80) != 0;
        
        if (used && !is_dir) {  // File 
            int start = temp_sb->inode[i].start_block;
            int size = temp_sb->inode[i].isused_size & 0x7F;
            
            // Check start block in valid range
            if (start < 1 || start > 127) {
                return 2;  // start_block out of range
            }
            
            // Check last block in valid range
            if (start + size - 1 > 127) {
                return 2;  // file extends beyond disk
            }
        }
    }
    
    /* ====================================================================
     * CHECK 3: Directory metadata validation
     * - Directories must have size = 0 and start_block = 0
     * - They don't occupy data blocks 
     * ==================================================================== */
    for (int i = 0; i < 126; i++) {
        int used = (temp_sb->inode[i].isused_size & 0x80) != 0;
        int is_dir = (temp_sb->inode[i].isdir_parent & 0x80) != 0;
        
        if (used && is_dir) {
            int size = temp_sb->inode[i].isused_size & 0x7F;
            int start = temp_sb->inode[i].start_block;
            
            if (size != 0 || start != 0) {
                return 3;  // directory has non-zero size/start
            }
        }
    }
    
    /* ====================================================================
     * CHECK 4: Parent index validation
     * - Parent index cannot equal own index (no self-parenting)
     * - Parent index cannot be 126 (reserved value)
     * - If parent is 0-125, parent inode must be used and be a directory
     * - If parent is 127, it's the root (always valid)
     * ==================================================================== */
    for (int i = 0; i < 126; i++) {
        int used = (temp_sb->inode[i].isused_size & 0x80) != 0;
        
        if (used) {
            int parent = temp_sb->inode[i].isdir_parent & 0x7F;
            
            // Check: can't be own parent
            if (parent == i) {
                return 4;  // self-parenting
            }
            
            // Check: 126 is reserved
            if (parent == 126) {
                return 4;  // invalid parent value
            }
            
            // If parent is regular inode (not root 127)
            if (parent <= 125) {
                int parent_used = (temp_sb->inode[parent].isused_size & 0x80) != 0;
                int parent_is_dir = (temp_sb->inode[parent].isdir_parent & 0x80) != 0;
                
                // Parent must exist and be a directory
                if (!parent_used || !parent_is_dir) {
                    return 4;  // invalid parent
                }
            }
        }
    }
    
    /* ====================================================================
     * CHECK 5: Name uniqueness within directories
     *
     * - Within each directory, all file/directory names must be unique
     * - Case-insensitive comparison (strcasecmp)
     * - Names can be duplicated across DIFFERENT directories
     * 
     * - For each directory (including root = 127)
     * - Compare all pairs of children in that directory
     * ==================================================================== */
    
    // Check root directory (parent = 127)
    for (int i = 0; i < 126; i++) {
        int used_i = (temp_sb->inode[i].isused_size & 0x80) != 0;
        if (!used_i) continue;
        
        int parent_i = temp_sb->inode[i].isdir_parent & 0x7F;
        if (parent_i != 127) continue;  // Not in root
        
        // Extract name of inode i
        char name_i[6];
        memcpy(name_i, temp_sb->inode[i].name, 5);
        name_i[5] = '\0';
        
        // Compare with all subsequent inodes in root
        for (int j = i + 1; j < 126; j++) {
            int used_j = (temp_sb->inode[j].isused_size & 0x80) != 0;
            if (!used_j) continue;
            
            int parent_j = temp_sb->inode[j].isdir_parent & 0x7F;
            if (parent_j != 127) continue;
            
            char name_j[6];
            memcpy(name_j, temp_sb->inode[j].name, 5);
            name_j[5] = '\0';
            
            if (strcasecmp(name_i, name_j) == 0) {
                return 5;  // duplicate name in directory
            }
        }
    }
    
    // Check all other directories (parent = 0-125)
    for (int dir = 0; dir < 126; dir++) {
        int used_dir = (temp_sb->inode[dir].isused_size & 0x80) != 0;
        int is_dir = (temp_sb->inode[dir].isdir_parent & 0x80) != 0;
        
        if (!used_dir || !is_dir) continue;  // Skip if not a directory
        
        // Check all children of this directory
        for (int i = 0; i < 126; i++) {
            int used_i = (temp_sb->inode[i].isused_size & 0x80) != 0;
            if (!used_i) continue;
            
            int parent_i = temp_sb->inode[i].isdir_parent & 0x7F;
            if (parent_i != dir) continue;
            
            char name_i[6];
            memcpy(name_i, temp_sb->inode[i].name, 5);
            name_i[5] = '\0';
            
            for (int j = i + 1; j < 126; j++) {
                int used_j = (temp_sb->inode[j].isused_size & 0x80) != 0;
                if (!used_j) continue;
                
                int parent_j = temp_sb->inode[j].isdir_parent & 0x7F;
                if (parent_j != dir) continue;
                
                char name_j[6];
                memcpy(name_j, temp_sb->inode[j].name, 5);
                name_j[5] = '\0';
                
                if (strcasecmp(name_i, name_j) == 0) {
                    return 5;  // duplicate name
                }
            }
        }
    }
    
    /* ====================================================================
     * CHECK 6: Block allocation consistency
     *
     * - Blocks marked FREE (bit=0) must not be allocated to any file
     * - Blocks marked USED (bit=1) must be allocated to EXACTLY one file
     * 
     * - Count allocations per block across all files
     * - Compare with free-space list
     * ==================================================================== */
    int block_count[128] = {0};

    block_count[0] = 1;  // Superblock is always used
    
    // Count how many times each block is allocated
    for (int i = 0; i < 126; i++) {
        int used = (temp_sb->inode[i].isused_size & 0x80) != 0;
        int is_dir = (temp_sb->inode[i].isdir_parent & 0x80) != 0;
        
        if (used && !is_dir) {  // File
            int start = temp_sb->inode[i].start_block;
            int size = temp_sb->inode[i].isused_size & 0x7F;
            
            for (int b = start; b < start + size; b++) {
                block_count[b]++;
            }
        }
    }
    
    // Verify consistency with free-space list
    for (int b = 0; b < 128; b++) {
        int is_free = is_block_free_from_sb(temp_sb, b);
        
        if (is_free && block_count[b] > 0) {
            return 6;  //free block is allocated
        }
        
        if (!is_free && block_count[b] != 1) {
            return 6;  //used block not allocated exactly once
        }
    }
    
    return 0;  // All checks passed successfully yayyyyyy woohoo
}

/* ========================================================================
 * MAIN FILESYSTEM OPERATIONS
 * ======================================================================== */

/**
 * Mounts a virtual disk and validates its filesystem.
 *  Buffer is NOT zeroed on mount
 */
void fs_mount(char *name) {
    /* Check if disk file exists */
    int fd = open(name, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Error: Cannot find disk %s\n", name); //File doesn't exist
        return;
    }
    
    /* Read superblock from disk */
    Superblock temp_sb;
    ssize_t bytes_read = read(fd, &temp_sb, sizeof(Superblock));
    
    if (bytes_read != sizeof(Superblock)) {
        fprintf(stderr, "Error: Cannot find disk %s\n", name); //Failed to read superblock
        close(fd);
        return;
    }
    
    /* Run consistency checks */
    int error_code = check_consistency(&temp_sb);
    
    if (error_code > 0) {
        /* Filesystem is inconsistent - don't mount */
        fprintf(stderr, "Error: File system in %s is inconsistent (error code: %d)\n", //Checks failed (nooo)
                name, error_code);
        close(fd);
        return;
    }
    
    /* Success - mount the filesystem */
    
    // Close previous disk if one was mounted
    if (is_mounted && disk_fd >= 0) {
        close(disk_fd);
    }
    
    // Update global state
    memcpy(&sb, &temp_sb, sizeof(Superblock));
    strncpy(current_disk_name, name, sizeof(current_disk_name) - 1);
    current_disk_name[sizeof(current_disk_name) - 1] = '\0';
    disk_fd = fd;
    is_mounted = 1;
    current_dir_inode = 127;  // Set CWD to root
}

/**
 * Creates a new file or directory.
 */
void fs_create(char name[5], int size) {
    /* Check if filesystem is mounted */
    if (!is_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    
    /*  Check if Free inode available */
    int inode_idx = find_free_inode();
    if (inode_idx < 0) {
        fprintf(stderr, "Error: Superblock in disk %s is full, cannot create %s\n",
                current_disk_name, name);
        return;
    }
    
    /* Name uniqueness Check */
    
    // Check for reserved names . and ..
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        fprintf(stderr, "Error: File or directory %s already exists\n", name);
        return;
    }
    
    // Check for duplicate in current directory
    if (find_inode_by_name(name, current_dir_inode) >= 0) {
        fprintf(stderr, "Error: File or directory %s already exists\n", name);
        return;
    }
    
    /* Contiguous blocks available (for files only) */
    int start_block = 0;
    if (size > 0) {  // File (not directory)
        start_block = find_contiguous_blocks(size);
        if (start_block < 0) {
            fprintf(stderr, "Error: Cannot allocate %d blocks on %s\n",
                    size, current_disk_name);
            return;
        }
    }
    
    /* create the inode */
    Inode *inode = &sb.inode[inode_idx];
    memset(inode, 0, sizeof(Inode));
    
    set_inode_name(inode, name);
    set_inode_used(inode, 1);
    set_parent_index(inode, current_dir_inode);
    
    if (size == 0) {
        /* Creating a directory */
        set_is_directory(inode, 1);
        set_file_size(inode, 0);
        inode->start_block = 0;
    } else {
        /* Creating a file */
        set_is_directory(inode, 0);
        set_file_size(inode, size);
        inode->start_block = start_block;
        
        // Mark blocks as used in free-space list
        mark_blocks_used(start_block, size, 1);
        
        // Zero the allocated blocks
        uint8_t zero_buf[1024] = {0};
        for (int i = 0; i < size; i++) {
            write_block(start_block + i, zero_buf);
        }
    }
    
    /* Write superblock back to disk */
    save_superblock();
}

/**
 * Helper function to recursively delete an inode and its children.
 * - If directory: recursively delete all children first
 * - If file: zero and free data blocks
 * - Zero the inode itself
 */
static void recursive_delete(int inode_idx) {
    if (!is_inode_used(&sb.inode[inode_idx])) {
        return;  // Already free
    }
    
    if (is_directory(&sb.inode[inode_idx])) {
        /* Directory: recursively delete all children */
        for (int i = 0; i < 126; i++) {
            if (is_inode_used(&sb.inode[i]) &&
                get_parent_index(&sb.inode[i]) == inode_idx) {
                recursive_delete(i);
            }
        }
    } else {
        /* File: zero and free data blocks */
        int start = sb.inode[inode_idx].start_block;
        int size = get_file_size(&sb.inode[inode_idx]);
        
        uint8_t zero_buf[1024] = {0};
        for (int i = 0; i < size; i++) {
            write_block(start + i, zero_buf);
        }
        
        // Mark blocks as free
        mark_blocks_used(start, size, 0);
    }
    
    /* Zero the inode */
    memset(&sb.inode[inode_idx], 0, sizeof(Inode));
}

/**
 * Deletes a file or directory (and all its contents).
 */
void fs_delete(char name[5]) {
    if (!is_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    
    /* Find the inode */
    int inode_idx = find_inode_by_name(name, current_dir_inode);
    if (inode_idx < 0) {
        fprintf(stderr, "Error: File or directory %s does not exist\n", name);
        return;
    }
    
    /* Recursively delete */
    recursive_delete(inode_idx);
    
    /* Write superblock back to disk */
    save_superblock();
}

/**
 * Reads a block from a file into the buffer. * 
 */
void fs_read(char name[5], int block_num) {
    if (!is_mounted) { //Check if filesystem is mounted
        fprintf(stderr, "Error: No file system is mounted\n"); 
        return;
    }
    
    /* Find the file */
    int inode_idx = find_inode_by_name(name, current_dir_inode);
    
    // Check: file must exist and must not be a directory
    if (inode_idx < 0 || is_directory(&sb.inode[inode_idx])) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }
    
    /* Validate block number is in range [0, size-1] */
    int size = get_file_size(&sb.inode[inode_idx]);
    
    if (block_num < 0 || block_num >= size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }
    
    /* Read the block into global buffer */
    int start_block = sb.inode[inode_idx].start_block;
    int actual_block = start_block + block_num;
    
    read_block(actual_block, buffer);
}

/**
 * Writes the buffer to a block in a file.
 * Superblock doesn't need updating (no metadata changes)
 */
void fs_write(char name[5], int block_num) {
    if (!is_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    
    /* Find the file */
    int inode_idx = find_inode_by_name(name, current_dir_inode);
    
    // Check: file must exist and must not be a directory
    if (inode_idx < 0 || is_directory(&sb.inode[inode_idx])) {
        fprintf(stderr, "Error: File %s does not exist\n", name);
        return;
    }
    
    /* Validate block number */
    int size = get_file_size(&sb.inode[inode_idx]);
    
    if (block_num < 0 || block_num >= size) {
        fprintf(stderr, "Error: %s does not have block %d\n", name, block_num);
        return;
    }
    
    /* Write buffer to the block */
    int start_block = sb.inode[inode_idx].start_block;
    int actual_block = start_block + block_num;
    
    write_block(actual_block, buffer);
}

/**
 * Updates the buffer with new data.
 * - Remaining bytes stay zero if buff < 1024 bytes
 * -  No error checking required per spec
 */
void fs_buff(uint8_t buff[1024]) {

    /* CHECK: Filesystem must be mounted */
    if (!is_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    // Zero the buffer first
    memset(buffer, 0, 1024);
    
    // Copy new data
    memcpy(buffer, buff, 1024);
}

/**
 * Lists all files and directories in current working directory.
 * - Directory count includes . and .. (add 2 to actual children)
 */
void fs_ls(void) {
    // Check: Filesystem must be mounted
    if (!is_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    
    /* Count items in current directory */
    int current_count = count_children(current_dir_inode);
    
    /* Print . (current directory) */
    printf("%-5s %3d\n", ".", current_count);
    
    /* Print .. (parent directory) */
    if (current_dir_inode == 127) {
        // Root directory: .. is same as .
        printf("%-5s %3d\n", "..", current_count);
    } else {
        // Regular directory: .. is parent
        int parent_idx = get_parent_index(&sb.inode[current_dir_inode]);
        int parent_count = count_children(parent_idx);
        printf("%-5s %3d\n", "..", parent_count);
    }
    
    /* Print all entries in current directory, sorted by inode index */
    for (int i = 0; i < 126; i++) {
        if (is_inode_used(&sb.inode[i]) &&
            get_parent_index(&sb.inode[i]) == current_dir_inode) {
            
            char name[6];
            get_inode_name(&sb.inode[i], name);
            
            if (is_directory(&sb.inode[i])) {
                /* Directory: show count of children */
                int child_count = count_children(i);
                printf("%-5s %3d\n", name, child_count);
            } else {
                /* File: show size in KB */
                int size = get_file_size(&sb.inode[i]);
                printf("%-5s %3d KB\n", name, size);
            }
        }
    }
}

/**
 * Changes current working directory.
 * - "." stays in current directory
 * - ".." moves to parent (unless already at root)
 * - Other names must be subdirectories in current directory
 * - If at root and user does "cd ..", stay at root (don't error)
 */
void fs_cd(char name[5]) {
    if (!is_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }
    
    /* Handle special case: "." (stay in current directory) */
    if (strcmp(name, ".") == 0) {
        return;  // No change
    }
    
    /* Handle special case: ".." (move to parent) */
    if (strcmp(name, "..") == 0) {
        if (current_dir_inode == 127) {
            // Already at root - stay at root
            return;
        } else {
            // Move to parent
            int parent_idx = get_parent_index(&sb.inode[current_dir_inode]);
            current_dir_inode = parent_idx;
            return;
        }
    }
    
    /* Regular directory name - find it in current directory */
    int inode_idx = find_inode_by_name(name, current_dir_inode);
    
    // Check: must exist and must be a directory
    if (inode_idx < 0 || !is_directory(&sb.inode[inode_idx])) {
        fprintf(stderr, "Error: Directory %s does not exist\n", name);
        return;
    }
    
    /* Change to the new directory */
    current_dir_inode = inode_idx;
}

/**
 * Defragments the disk by moving all used blocks to be contiguous.
 * - All used blocks are packed at beginning (after superblock)
 * - All free blocks are at end
 * - File data is preserved
 */
void fs_defrag(void) {
    if (!is_mounted) {
        fprintf(stderr, "Error: No file system is mounted\n");
        return;
    }

    //Structure to hold file info for sorting
    typedef struct {
        int inode_idx;
        int start_block;
        int size;
    } FileInfo;
    
    FileInfo files[126];
    int file_count = 0;
    
    // Collect all files 
    for (int i = 0; i < 126; i++) {
        if (is_inode_used(&sb.inode[i]) && !is_directory(&sb.inode[i])) {
            files[file_count].inode_idx = i;
            files[file_count].start_block = sb.inode[i].start_block;
            files[file_count].size = get_file_size(&sb.inode[i]);
            file_count++;
        }
    }
    
    // Sort files by start_block (simple bubble sort)
    for (int i = 0; i < file_count - 1; i++) {
        for (int j = i + 1; j < file_count; j++) {
            if (files[j].start_block < files[i].start_block) {
                FileInfo temp = files[i];
                files[i] = files[j];
                files[j] = temp;
            }
        }
    }
    
    // Compact files to start of disk
    uint8_t temp_buf[1024];
    int next_free = 1;  // Start at block 1 (block 0 is superblock)
    
    for (int i = 0; i < file_count; i++) {
        int inode_idx = files[i].inode_idx;
        int old_start = sb.inode[inode_idx].start_block;
        int size = files[i].size;
        
        if (old_start != next_free) {
            /* File needs to be moved */
            
            // Copy blocks from old location to new location
            for (int b = 0; b < size; b++) {
                read_block(old_start + b, temp_buf);
                write_block(next_free + b, temp_buf);
            }
            
            // Update inode with new start_block
            sb.inode[inode_idx].start_block = next_free;
        }
        
        // Advance next_free pointer
        next_free += size;
    }
    
     // Clear remaining blocks on disk
     memset(temp_buf, 0, 1024);
    for (int b = next_free; b < 128; b++) {
        write_block(b, temp_buf);
    }
    
    // Clear free-space list (all blocks initially free)
    memset(sb.free_block_list, 0, 16);
    
    // Mark superblock as used
    set_block_free(0, 0);
    
    // Mark all file blocks as used
    for (int i = 0; i < file_count; i++) {
        int inode_idx = files[i].inode_idx;
        int start = sb.inode[inode_idx].start_block;
        int size = files[i].size;
        
        mark_blocks_used(start, size, 1);
    }
    
    /* Write updated superblock to disk */
    save_superblock();
}

/**
 * Parses a single line and executes the corresponding command.
 * 
 * @param line: Input line from command file
 * @param line_num: Line number (for error reporting)
 * @param input_file: Input filename (for error reporting)
 * @return: 1 if command parsed successfully, 0 if error
 * 
 * COMMAND FORMAT:
 * M <disk_name>                  - Mount
 * C <file_name> <size>           - Create
 * D <file_name>                  - Delete
 * R <file_name> <block_num>      - Read
 * W <file_name> <block_num>      - Write
 * B <characters>                 - Buffer update
 * L                              - List
 * O                              - Defragment
 * Y <directory_name>             - Change directory
 * 
 * ERRORS
 * - Invalid command letter
 * - Wrong number of arguments
 * - Invalid argument format
 * - Argument out of range
 */
static int parse_and_execute_command(char *line, int line_num, const char *input_file) {
    // 1. Robust check for empty or whitespace-only lines
    int i = 0;
    while (isspace((unsigned char)line[i])) i++;
    if (line[i] == '\0') {
        return 1; // Successfully skipped empty line
    }
    
    char cmd;
    char arg1[256];
    char extra[256]; // Buffer to detect unexpected extra arguments
    int num_arg;
    
    /* Remove trailing newline safely */
    char *newline = strchr(line, '\n');
    if (newline) *newline = '\0';
    
    /* Parse command letter */
    if (sscanf(line, " %c", &cmd) != 1) {
        fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
        return 0;
    }
    
    /* Advance pointer to arguments */
    char *args = line;
    while (isspace((unsigned char)*args)) args++; // skip leading space
    if (*args == cmd) args++; // skip cmd char
    while (isspace((unsigned char)*args)) args++; // skip space after cmd
    
    switch (cmd) {
        case 'M': {
            if (sscanf(args, "%s %s", arg1, extra) != 1) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            fs_mount(arg1);
            break;
        }
        case 'C': {
            if (sscanf(args, "%s %d %s", arg1, &num_arg, extra) != 2) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            // Constraint checks
            if (strlen(arg1) > 5 || num_arg < 0 || num_arg > 127) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            fs_create(arg1, num_arg);
            break;
        }
        case 'D': {
            if (sscanf(args, "%s %s", arg1, extra) != 1) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            if (strlen(arg1) > 5) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            fs_delete(arg1);
            break;
        }
        case 'R': {
            if (sscanf(args, "%s %d %s", arg1, &num_arg, extra) != 2) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            if (strlen(arg1) > 5 || num_arg < 0 || num_arg > 126) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            fs_read(arg1, num_arg);
            break;
        }
        case 'W': {
            if (sscanf(args, "%s %d %s", arg1, &num_arg, extra) != 2) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            if (strlen(arg1) > 5 || num_arg < 0 || num_arg > 126) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            fs_write(arg1, num_arg);
            break;
        }
        case 'B': {
            if (*args == '\0') {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            // Check length limit
            if (strlen(args) > 1024) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            uint8_t new_buf[1024] = {0};
            memcpy(new_buf, args, strlen(args));
            fs_buff(new_buf);
            break;
        }
        case 'L': {
            if (*args != '\0') {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            fs_ls();
            break;
        }
        case 'O': {
            if (*args != '\0') {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            fs_defrag();
            break;
        }
        case 'Y': {
            if (sscanf(args, "%s %s", arg1, extra) != 1) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            if (strlen(arg1) > 5) {
                fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
                return 0;
            }
            fs_cd(arg1);
            break;
        }
        default:
            fprintf(stderr, "Command Error: %s, %d\n", input_file, line_num);
            return 0;
    }
    return 1;
}

int main(int argc, char *argv[]) {
    /* Validate command-line arguments */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input_file>\n", argv[0]);
        return 1;
    }
    
    /* Open input file */
    FILE *input = fopen(argv[1], "r");
    if (!input) {
        fprintf(stderr, "Error: Cannot open input file %s\n", argv[1]);
        return 1;
    }
    
    /* Initialize buffer to zero */
    memset(buffer, 0, 1024);
    
    /* Read and execute commands line by line */
    char line[2048];
    int line_num = 0;
    
    while (fgets(line, sizeof(line), input)) {
        line_num++;
        parse_and_execute_command(line, line_num, argv[1]);
    }
    
    /* Cleanup */
    fclose(input);
    if (is_mounted && disk_fd >= 0) {
        close(disk_fd);
    }
    
    return 0;
}