/*
 * CMPUT 379 Assignment 3 - Header File
 * Name: Chidinma Obi-Okoye
 * CCID: obiokoye
 */

#ifndef FS_SIM_H
#define FS_SIM_H

#include <stdint.h>

/**
 * Inode Structure (8 bytes)
 * Packed to prevent compiler padding
 * * Bit packing details:
 * isused_size:  bit 7 = status (1=used), bits 0-6 = size
 * isdir_parent: bit 7 = type (1=dir),    bits 0-6 = parent index
 */
 typedef struct __attribute__((packed)){
    char name[5];         // name of the file/directory
    uint8_t isused_size;  // state of inode and size of the file/directory
    uint8_t start_block;  // index of the first block of the file/directory
    uint8_t isdir_parent; // type of inode and index of the parent inode
} Inode;

/**
 * Superblock Structure (1024 bytes)
 * Fits in block 0 of the disk
 * - 0 = free, 1 = used
 * - Stored as byte array (little-endian bit ordering within each byte)
 */
typedef struct __attribute__((packed)) {
    uint8_t free_block_list[16];
    Inode inode[126];
} Superblock;

/* Function prototypes - implementations in fs-sim.c */
void fs_mount(char *new_disk_name);
void fs_create(char name[5], int size);
void fs_delete(char name[5]);
void fs_read(char name[5], int block_num);
void fs_write(char name[5], int block_num);
void fs_buff(uint8_t buff[1024]);
void fs_ls(void);
void fs_defrag(void);
void fs_cd(char name[5]);

#endif /* FS_SIM_H */