#include "ext2.h"
#include "disk.h"
#include "vga.h"
#include <stdint.h>
#include <stddef.h>

extern void* memcpy(void* dest, const void* src, size_t n);
extern void* memset(void* s, int c, size_t n);
extern int strcmp(const char* a, const char* b);
extern size_t strlen(const char* s);
extern void putc(char c);

#define EXT2_MAX_BLOCK_SIZE 1024
#define EXT2_S_IFMT 0xF000
#define EXT2_FT_REG_FILE 1

static struct ext2_superblock sb;
static uint32_t block_size = 1024;
static uint32_t blocks_per_group = 8192;
static uint32_t inodes_per_group = 8192;
static uint16_t inode_size = 128;

int ext2_read_block(uint32_t block_num, uint8_t* buffer);
int ext2_write_block(uint32_t block_num, const uint8_t* buffer);

static uint16_t align4(uint16_t value) {
    return (value + 3) & ~3;
}

static uint32_t ext2_bgdt_block() {
    return (block_size == 1024) ? 2 : 1;
}

static int ext2_read_superblock_block(uint8_t* buffer) {
    if (disk_read_sector(2, buffer) != 0) return -1;
    if (disk_read_sector(3, buffer + 512) != 0) return -1;
    return 0;
}

static int ext2_write_superblock_block(const uint8_t* buffer) {
    if (disk_write_sector(2, buffer) != 0) return -1;
    if (disk_write_sector(3, buffer + 512) != 0) return -1;
    return 0;
}

static int ext2_read_group_desc(uint32_t group, struct ext2_group_desc* bgd) {
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];
    uint32_t offset = group * sizeof(struct ext2_group_desc);

    if (ext2_read_block(ext2_bgdt_block(), block_buf) != 0) {
        return -1;
    }

    memcpy(bgd, block_buf + offset, sizeof(struct ext2_group_desc));
    return 0;
}

static int ext2_write_group_desc(uint32_t group, const struct ext2_group_desc* bgd) {
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];
    uint32_t offset = group * sizeof(struct ext2_group_desc);

    if (ext2_read_block(ext2_bgdt_block(), block_buf) != 0) {
        return -1;
    }

    memcpy(block_buf + offset, bgd, sizeof(struct ext2_group_desc));
    return ext2_write_block(ext2_bgdt_block(), block_buf);
}

static int ext2_flush_metadata(uint32_t group, const struct ext2_group_desc* bgd) {
    uint8_t super_buf[EXT2_MAX_BLOCK_SIZE];

    memset(super_buf, 0, sizeof(super_buf));
    if (ext2_read_superblock_block(super_buf) != 0) {
        return -1;
    }

    memcpy(super_buf, &sb, sizeof(struct ext2_superblock));
    if (ext2_write_superblock_block(super_buf) != 0) {
        return -1;
    }

    return ext2_write_group_desc(group, bgd);
}

static int ext2_bitmap_allocate(uint32_t bitmap_block, uint32_t limit, uint32_t* bit_out) {
    uint8_t bitmap[EXT2_MAX_BLOCK_SIZE];

    if (ext2_read_block(bitmap_block, bitmap) != 0) {
        return -1;
    }

    for (uint32_t bit = 0; bit < limit; bit++) {
        uint32_t byte_index = bit / 8;
        uint8_t mask = 1u << (bit % 8);
        if ((bitmap[byte_index] & mask) == 0) {
            bitmap[byte_index] |= mask;
            if (ext2_write_block(bitmap_block, bitmap) != 0) {
                return -1;
            }
            *bit_out = bit;
            return 0;
        }
    }

    return -1;
}

static int ext2_allocate_block(uint32_t* block_num_out) {
    struct ext2_group_desc bgd;
    uint32_t bit_index;

    if (ext2_read_group_desc(0, &bgd) != 0) {
        return -1;
    }

    if (ext2_bitmap_allocate(bgd.block_bitmap, blocks_per_group, &bit_index) != 0) {
        return -1;
    }

    if (sb.free_blocks > 0) sb.free_blocks--;
    if (bgd.free_blocks_count > 0) bgd.free_blocks_count--;

    if (ext2_flush_metadata(0, &bgd) != 0) {
        return -1;
    }

    *block_num_out = sb.first_data_block + bit_index;
    return 0;
}

static int ext2_allocate_inode(uint32_t* inode_num_out) {
    struct ext2_group_desc bgd;
    uint32_t bit_index;

    if (ext2_read_group_desc(0, &bgd) != 0) {
        return -1;
    }

    if (ext2_bitmap_allocate(bgd.inode_bitmap, inodes_per_group, &bit_index) != 0) {
        return -1;
    }

    if (sb.free_inodes > 0) sb.free_inodes--;
    if (bgd.free_inodes_count > 0) bgd.free_inodes_count--;

    if (ext2_flush_metadata(0, &bgd) != 0) {
        return -1;
    }

    *inode_num_out = bit_index + 1;
    return 0;
}

static int ext2_write_inode(uint32_t inode_num, const struct ext2_inode* inode) {
    struct ext2_group_desc bgd;
    uint32_t group;
    uint32_t local_inode;
    uint32_t inode_block;
    uint32_t inode_offset;
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];

    if (inode_num == 0) {
        return -1;
    }

    group = (inode_num - 1) / inodes_per_group;
    local_inode = (inode_num - 1) % inodes_per_group;

    if (ext2_read_group_desc(group, &bgd) != 0) {
        return -1;
    }

    inode_block = bgd.inode_table + (local_inode * inode_size) / block_size;
    inode_offset = (local_inode * inode_size) % block_size;

    if (ext2_read_block(inode_block, block_buf) != 0) {
        return -1;
    }

    memcpy(block_buf + inode_offset, inode, sizeof(struct ext2_inode));
    return ext2_write_block(inode_block, block_buf);
}

static uint16_t ext2_dirent_actual_length(uint8_t name_len) {
    return 8 + align4(name_len);
}

static int ext2_add_dirent(uint32_t dir_inode_num, uint32_t child_inode_num, const char* name, uint8_t type) {
    struct ext2_inode dir_inode;
    uint32_t name_len = (uint32_t)strlen(name);
    uint16_t needed = ext2_dirent_actual_length((uint8_t)name_len);
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];

    if (name_len == 0 || name_len > EXT2_NAME_LEN) {
        return -1;
    }

    if (ext2_read_inode(dir_inode_num, &dir_inode) != 0) {
        return -1;
    }

    for (uint32_t block_idx = 0; block_idx < 12; block_idx++) {
        if (dir_inode.block[block_idx] == 0) {
            uint32_t new_block;
            struct ext2_dirent* new_entry;

            if (ext2_allocate_block(&new_block) != 0) {
                return -1;
            }

            memset(block_buf, 0, sizeof(block_buf));
            new_entry = (struct ext2_dirent*)block_buf;
            new_entry->inode = child_inode_num;
            new_entry->rec_len = block_size;
            new_entry->name_len = (uint8_t)name_len;
            new_entry->type = type;
            memcpy(new_entry->name, name, name_len);

            dir_inode.block[block_idx] = new_block;
            dir_inode.size += block_size;
            dir_inode.blocks += block_size / 512;

            if (ext2_write_block(new_block, block_buf) != 0) {
                return -1;
            }

            return ext2_write_inode(dir_inode_num, &dir_inode);
        }

        if (ext2_read_block(dir_inode.block[block_idx], block_buf) != 0) {
            return -1;
        }

        for (uint32_t offset = 0; offset < block_size;) {
            struct ext2_dirent* entry = (struct ext2_dirent*)(block_buf + offset);
            uint16_t actual_len;

            if (entry->rec_len == 0) {
                break;
            }

            actual_len = ext2_dirent_actual_length(entry->name_len);
            if (entry->rec_len >= actual_len + needed) {
                struct ext2_dirent* new_entry = (struct ext2_dirent*)(block_buf + offset + actual_len);
                uint16_t remaining = entry->rec_len - actual_len;

                entry->rec_len = actual_len;
                new_entry->inode = child_inode_num;
                new_entry->rec_len = remaining;
                new_entry->name_len = (uint8_t)name_len;
                new_entry->type = type;
                memcpy(new_entry->name, name, name_len);

                return ext2_write_block(dir_inode.block[block_idx], block_buf);
            }

            offset += entry->rec_len;
        }
    }

    return -1;
}

int ext2_mount() {
    uint8_t sb_buf[EXT2_MAX_BLOCK_SIZE];

    memset(sb_buf, 0, sizeof(sb_buf));
    if (ext2_read_superblock_block(sb_buf) != 0) {
        return -1;
    }

    memcpy(&sb, sb_buf, sizeof(struct ext2_superblock));

    if (sb.magic != 0xEF53) {
        print("[EXT2] Invalid superblock magic: ");
        print_hex(sb.magic);
        print("\n");
        return -1;
    }

    block_size = 1024 << sb.log_block_size;
    if (block_size > EXT2_MAX_BLOCK_SIZE) {
        print("[EXT2] Unsupported block size\n");
        return -1;
    }

    blocks_per_group = sb.blocks_per_group;
    inodes_per_group = sb.inodes_per_group;
    if (sb.inode_size != 0) {
        inode_size = sb.inode_size;
    }

    print("[EXT2] Filesystem mounted\n");
    print("[EXT2] Block size: ");
    print_int(block_size);
    print(" bytes\n");
    print("[EXT2] Total blocks: ");
    print_int(sb.total_blocks);
    print(", Free blocks: ");
    print_int(sb.free_blocks);
    print("\n");
    
    return 0;
}

int ext2_read_block(uint32_t block_num, uint8_t* buffer) {
    uint32_t sectors_to_read = block_size / 512;
    uint32_t start_sector = block_num * sectors_to_read;

    for (uint32_t i = 0; i < sectors_to_read; i++) {
        if (disk_read_sector(start_sector + i, buffer + i * 512) != 0) {
            return -1;
        }
    }

    return 0;
}

int ext2_write_block(uint32_t block_num, const uint8_t* buffer) {
    uint32_t sectors_to_write = block_size / 512;
    uint32_t start_sector = block_num * sectors_to_write;

    for (uint32_t i = 0; i < sectors_to_write; i++) {
        if (disk_write_sector(start_sector + i, buffer + i * 512) != 0) {
            return -1;
        }
    }

    return 0;
}

int ext2_read_inode(uint32_t inode_num, struct ext2_inode* inode) {
    if (inode_num == 0) return -1;

    uint32_t bg = (inode_num - 1) / inodes_per_group;
    uint32_t local_inode = (inode_num - 1) % inodes_per_group;
    struct ext2_group_desc bgd;
    uint32_t inode_block;
    uint32_t inode_offset;
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];

    if (ext2_read_group_desc(bg, &bgd) != 0) {
        return -1;
    }

    inode_block = bgd.inode_table + (local_inode * inode_size) / block_size;
    inode_offset = (local_inode * inode_size) % block_size;

    if (ext2_read_block(inode_block, block_buf) != 0) {
        return -1;
    }

    memcpy(inode, block_buf + inode_offset, sizeof(struct ext2_inode));

    return 0;
}

int ext2_read_file(uint32_t inode_num, uint32_t offset, char* buffer, uint32_t size) {
    struct ext2_inode inode;

    if (ext2_read_inode(inode_num, &inode) != 0) {
        return -1;
    }

    if (offset >= inode.size) {
        return 0;
    }

    if (offset + size > inode.size) {
        size = inode.size - offset;
    }

    uint32_t bytes_read = 0;
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];
    uint32_t block_idx = offset / block_size;
    uint32_t block_offset = offset % block_size;

    while (bytes_read < size && block_idx < 12) {
        if (inode.block[block_idx] == 0) {
            break;
        }

        if (ext2_read_block(inode.block[block_idx], block_buf) != 0) {
            return -1;
        }

        uint32_t to_copy = block_size - block_offset;
        if (to_copy > size - bytes_read) {
            to_copy = size - bytes_read;
        }

        memcpy(buffer + bytes_read, block_buf + block_offset, to_copy);
        bytes_read += to_copy;
        block_offset = 0;
        block_idx++;
    }

    return bytes_read;
}

static int ext2_find_in_dir(uint32_t dir_inode_num, const char* filename) {
    struct ext2_inode dir_inode;
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];
    uint32_t bytes_processed = 0;
    uint32_t block_idx = 0;
    size_t filename_len = strlen(filename);

    if (ext2_read_inode(dir_inode_num, &dir_inode) != 0) {
        return -1;
    }

    if ((dir_inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        return -1;
    }

    while (bytes_processed < dir_inode.size && block_idx < 12) {
        if (dir_inode.block[block_idx] == 0) {
            break;
        }

        if (ext2_read_block(dir_inode.block[block_idx], block_buf) != 0) {
            return -1;
        }

        for (uint32_t offset = 0; offset < block_size && bytes_processed < dir_inode.size;) {
            struct ext2_dirent* dirent = (struct ext2_dirent*)(block_buf + offset);

            if (dirent->inode == 0 || dirent->rec_len == 0) {
                break;
            }

            if (dirent->name_len == filename_len) {
                int match = 1;
                for (uint8_t i = 0; i < dirent->name_len; i++) {
                    if (dirent->name[i] != filename[i]) {
                        match = 0;
                        break;
                    }
                }
                if (match) {
                    return dirent->inode;
                }
            }

            offset += dirent->rec_len;
            bytes_processed += dirent->rec_len;
        }

        block_idx++;
    }

    return -1;
}

static int ext2_resolve_path(const char* path, uint32_t* inode_out) {
    uint32_t current = EXT2_ROOT_INO;
    char component[EXT2_NAME_LEN + 1];
    uint32_t component_len = 0;
    uint32_t index = 0;

    if (!path || path[0] == 0 || (path[0] == '/' && path[1] == 0)) {
        *inode_out = EXT2_ROOT_INO;
        return 0;
    }

    while (path[index]) {
        while (path[index] == '/') {
            index++;
        }
        if (!path[index]) {
            break;
        }

        component_len = 0;
        while (path[index] && path[index] != '/') {
            if (component_len >= EXT2_NAME_LEN) {
                return -1;
            }
            component[component_len++] = path[index++];
        }
        component[component_len] = 0;

        current = (uint32_t)ext2_find_in_dir(current, component);
        if (current == (uint32_t)-1) {
            return -1;
        }
    }

    *inode_out = current;
    return 0;
}

static int ext2_resolve_parent(const char* path, uint32_t* parent_inode_out, char* leaf_name_out) {
    uint32_t current = EXT2_ROOT_INO;
    char component[EXT2_NAME_LEN + 1];
    uint32_t component_len = 0;
    uint32_t index = 0;

    if (!path || path[0] == 0) {
        return -1;
    }

    while (path[index] == '/') {
        index++;
    }

    if (!path[index]) {
        return -1;
    }

    while (path[index]) {
        while (path[index] == '/') {
            index++;
        }
        if (!path[index]) {
            break;
        }

        component_len = 0;
        while (path[index] && path[index] != '/') {
            if (component_len >= EXT2_NAME_LEN) {
                return -1;
            }
            component[component_len++] = path[index++];
        }
        component[component_len] = 0;

        while (path[index] == '/') {
            index++;
        }

        if (!path[index]) {
            memcpy(leaf_name_out, component, component_len + 1);
            *parent_inode_out = current;
            return 0;
        }

        current = (uint32_t)ext2_find_in_dir(current, component);
        if (current == (uint32_t)-1) {
            return -1;
        }
    }

    return -1;
}

int ext2_list_dir(uint32_t inode_num) {
    struct ext2_inode inode;
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];
    uint32_t bytes_processed = 0;
    uint32_t block_idx = 0;

    if (ext2_read_inode(inode_num, &inode) != 0) {
        return -1;
    }

    if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFDIR) {
        return -1;
    }

    while (bytes_processed < inode.size && block_idx < 12) {
        if (inode.block[block_idx] == 0) {
            break;
        }

        if (ext2_read_block(inode.block[block_idx], block_buf) != 0) {
            return -1;
        }

        for (uint32_t offset = 0; offset < block_size && bytes_processed < inode.size;) {
            struct ext2_dirent* dirent = (struct ext2_dirent*)(block_buf + offset);

            if (dirent->inode == 0 || dirent->rec_len == 0) {
                break;
            }

            if (dirent->name_len > 0) {
                for (int i = 0; i < dirent->name_len; i++) {
                    char c = dirent->name[i];
                    if (c >= 32 && c < 127) {
                        putc(c);
                    }
                }
                print("\n");
            }

            offset += dirent->rec_len;
            bytes_processed += dirent->rec_len;
        }

        block_idx++;
    }

    return 0;
}

int ext2_list_path(const char* path) {
    uint32_t inode_num;

    if (ext2_resolve_path(path, &inode_num) != 0) {
        return -1;
    }

    return ext2_list_dir(inode_num);
}

int ext2_find_file(const char* filename) {
    return ext2_find_in_dir(EXT2_ROOT_INO, filename);
}

int ext2_find_path(const char* path) {
    uint32_t inode_num;

    if (ext2_resolve_path(path, &inode_num) != 0) {
        return -1;
    }

    return (int)inode_num;
}

int ext2_write_file(const char* path, const char* data, uint32_t size, uint32_t* inode_num_out) {
    struct ext2_inode inode;
    uint32_t parent_inode_num;
    uint32_t inode_num;
    uint32_t blocks_needed = (size + block_size - 1) / block_size;
    uint8_t block_buf[EXT2_MAX_BLOCK_SIZE];
    char leaf_name[EXT2_NAME_LEN + 1];
    int existing;

    if (blocks_needed > 12) {
        return -1;
    }

    if (ext2_resolve_parent(path, &parent_inode_num, leaf_name) != 0) {
        return -1;
    }

    existing = ext2_find_in_dir(parent_inode_num, leaf_name);
    inode_num = (existing >= 0) ? (uint32_t)existing : (uint32_t)-1;

    if (inode_num == (uint32_t)-1) {
        memset(&inode, 0, sizeof(inode));
        if (ext2_allocate_inode(&inode_num) != 0) {
            return -1;
        }
        inode.mode = EXT2_S_IFREG | 0644;
        inode.links_count = 1;
    } else {
        if (ext2_read_inode(inode_num, &inode) != 0) {
            return -1;
        }
        if ((inode.mode & EXT2_S_IFMT) != EXT2_S_IFREG) {
            return -1;
        }
    }

    for (uint32_t block_idx = 0; block_idx < blocks_needed; block_idx++) {
        uint32_t chunk_size = block_size;
        uint32_t allocated_block;

        if (inode.block[block_idx] == 0) {
            if (ext2_allocate_block(&allocated_block) != 0) {
                return -1;
            }
            inode.block[block_idx] = allocated_block;
        }

        if (size - block_idx * block_size < block_size) {
            chunk_size = size - block_idx * block_size;
        }

        memset(block_buf, 0, sizeof(block_buf));
        if (chunk_size > 0) {
            memcpy(block_buf, data + block_idx * block_size, chunk_size);
        }

        if (ext2_write_block(inode.block[block_idx], block_buf) != 0) {
            return -1;
        }
    }

    inode.size = size;
    inode.blocks = 0;
    for (uint32_t block_idx = 0; block_idx < 12; block_idx++) {
        if (inode.block[block_idx] != 0) {
            inode.blocks += block_size / 512;
        }
    }

    if (ext2_write_inode(inode_num, &inode) != 0) {
        return -1;
    }

    if (existing < 0) {
        if (ext2_add_dirent(parent_inode_num, inode_num, leaf_name, EXT2_FT_REG_FILE) != 0) {
            return -1;
        }
    }

    if (inode_num_out) {
        *inode_num_out = inode_num;
    }

    return 0;
}