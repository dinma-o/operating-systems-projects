typedef struct {
    char name[5];         // name of the file/directory
    uint8_t isused_size;  // state of inode and size of the file/directory
    uint8_t start_block;  // index of the first block of the file/directory
    uint8_t isdir_parent; // type of inode and index of the parent inode
} Inode;

typedef struct {
    uint8_t free_block_list[16];
    Inode inode[126];
} Superblock;

void fs_mount(char *new_disk_name);
void fs_create(char name[5], int size);
void fs_delete(char name[5]);
void fs_read(char name[5], int block_num);
void fs_write(char name[5], int block_num);
void fs_buff(uint8_t buff[1024]);
void fs_ls(void);
void fs_defrag(void);
void fs_cd(char name[5]);
