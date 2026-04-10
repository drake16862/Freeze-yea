#ifndef EXT2_H
#define EXT2_H

#include <stdint.h>

/* ext2 Superblock */
struct ext2_superblock {
    uint32_t total_inodes;
    uint32_t total_blocks;
    uint32_t reserved_blocks;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t first_data_block;
    uint32_t log_block_size;          /* 1024 << log_block_size = block size */
    uint32_t log_frag_size;
    uint32_t blocks_per_group;
    uint32_t frags_per_group;
    uint32_t inodes_per_group;
    uint32_t mtime;
    uint32_t wtime;
    uint16_t mount_count;
    uint16_t max_mount_count;
    uint16_t magic;                   /* 0xEF53 */
    uint16_t state;
    uint16_t errors;
    uint16_t minor_revision_level;
    uint32_t lastcheck;
    uint32_t checkinterval;
    uint32_t creator_os;
    uint32_t revision_level;
    uint16_t uid_reserved;
    uint16_t gid_reserved;
    uint32_t first_inode;
    uint16_t inode_size;
    uint16_t block_group_nr;
    uint32_t feature_compat;
    uint32_t feature_incompat;
    uint32_t feature_ro_compat;
    uint8_t uuid[16];
    char volume_name[16];
    char last_mounted[64];
} __attribute__((packed));

struct ext2_group_desc {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_blocks_count;
    uint16_t free_inodes_count;
    uint16_t used_dirs_count;
    uint16_t pad;
    uint8_t reserved[12];
} __attribute__((packed));

/* ext2 Inode */
struct ext2_inode {
    uint16_t mode;
    uint16_t uid;
    uint32_t size;
    uint32_t atime;
    uint32_t ctime;
    uint32_t mtime;
    uint32_t dtime;
    uint16_t gid;
    uint16_t links_count;
    uint32_t blocks;
    uint32_t flags;
    uint32_t osd1;
    uint32_t block[12];               /* Direct block pointers */
    uint32_t indirect_block;
    uint32_t double_indirect_block;
    uint32_t triple_indirect_block;
    uint32_t generation;
    uint32_t file_acl;
    uint32_t dir_acl;
    uint32_t faddr;
    uint8_t osd2[12];
} __attribute__((packed));

/* ext2 Directory Entry */
struct ext2_dirent {
    uint32_t inode;
    uint16_t rec_len;                 /* Record length */
    uint8_t name_len;
    uint8_t type;
    char name[256];
} __attribute__((packed));

/* Inode modes */
#define EXT2_S_IFREG 0x8000            /* Regular file */
#define EXT2_S_IFDIR 0x4000            /* Directory */

#define EXT2_NAME_LEN 255
#define EXT2_ROOT_INO 2                /* Root inode number */

int ext2_mount();
int ext2_read_inode(uint32_t inode_num, struct ext2_inode* inode);
int ext2_read_file(uint32_t inode_num, uint32_t offset, char* buffer, uint32_t size);
int ext2_list_dir(uint32_t inode_num);
int ext2_list_path(const char* path);
int ext2_find_file(const char* filename);
int ext2_find_path(const char* path);
int ext2_write_file(const char* path, const char* data, uint32_t size, uint32_t* inode_num_out);

#endif
