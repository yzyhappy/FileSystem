// fs.cpp: File System

#include "sfs/fs.h"

#include <algorithm>

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <memory>
#include <math.h>

// Debug file system -----------------------------------------------------------

void FileSystem::debug(Disk *disk) {
    Block block;

    // Read Superblock
    disk->read(0, block.Data);

    printf("SuperBlock:\n");
    printf("    %u blocks\n"         , block.Super.Blocks);
    printf("    %u inode blocks\n"   , block.Super.InodeBlocks);
    printf("    %u inodes\n"         , block.Super.Inodes);

    // Read Inode blocks

    /**
     * Inode在创建文件系统的时候就已经全部塞满Inode blocks，这是可以计算的
     * 区别只是用没用，所以在super block当中记录的应该是所有的Inode数量，包括没用的
     */

    int total_num = block.Super.InodeBlocks;

    for (uint32_t i = 1; i <= total_num; i++) {
        printf("Inode %d\n", i);
        disk->read(i, block.Data);
        uint64_t total_file_size = 0;
        uint32_t total_direct_blocks = 0;
        uint32_t total_indirect_blocks = 0;
        for (uint32_t j = 0; j < INODES_PER_BLOCK; j++) {
            if (block.Inodes[j].Valid) {
                total_file_size += block.Inodes[j].Size;
                for (uint32_t k = 0; k < POINTERS_PER_INODE; k++) {
                    if (block.Inodes[j].Direct[k]) total_direct_blocks++;
                }
                if (block.Inodes[j].Indirect) total_indirect_blocks++;
            }

        }


        printf("    size: %d\n", total_file_size);
        printf("    direct blocks: %d\n", total_direct_blocks);
        printf("    indirect blocks: %d\n", total_indirect_blocks);
    }

}

// Format file system ----------------------------------------------------------

bool FileSystem::format(Disk *disk) {
    // Write superblock
    std::shared_ptr<Block> block = std::make_shared<Block>();
    block->Super.Blocks = disk->size();
    block->Super.MagicNumber = MAGIC_NUMBER;
    // 按照我的理解，Inodes已经在这时被全部确定下来了
    block->Super.InodeBlocks = disk->size() / 10;
    block->Super.Inodes = INODES_PER_BLOCK * block->Super.InodeBlocks;

    // Clear all other blocks
    char tmp_data[Disk::BLOCK_SIZE];
    for (uint32_t i = 0; i < disk->size(); i++) {
        memset(tmp_data, '\0', sizeof(tmp_data));
        disk->write(i, tmp_data);
    }

    disk->write(0, block->Data);
    return true;
}

// Mount file system -----------------------------------------------------------

bool FileSystem::mount(Disk *disk) {
    // Read superblock
    Block block;
    disk->read(0, block.Data);

    // 比较magic number
    if (block.Super.MagicNumber != MAGIC_NUMBER) {
        return false;
    }


    // Set device and mount
    this->mountedOn = disk;

    // Copy metadata
    int sumOfBlocks = block.Super.Blocks;
    int sumOfInodeBlocks = block.Super.InodeBlocks;

    // Allocate free block bitmap

    int size = ceil(sumOfBlocks / 32.0);
    this->bitmap.resize(size, 0);

    //
    int offset = 0, index = 0;
    bitmap[0] = 1;
    for (int i = 1; i <= sumOfInodeBlocks; i++) {
        offset = i / 32;
        index = i % 32;
        bitmap[offset] |= (1 << index);
    }

    // Block *temp = new Block();
    std::shared_ptr<Block> tmp = std::make_shared<Block>();

    for (int i = 1; i <= sumOfInodeBlocks; i++) {
        disk->read(i, tmp->Data);

        for (auto inode : tmp->Inodes) {
            // 有效，则该inode已经被分配，需要查看磁盘情况
            if (inode.Valid == 1) {
                for (int k = 0; k < 5; k++) {
                    if (inode.Direct[k] != 0) {
                        offset = inode.Direct[k] / 32;
                        index = inode.Direct[k] % 32;

                        bitmap[offset] |= (1 << index);
                    }
                }

                // indirect 只考虑一级间接
                offset = inode.Indirect / 32;
                index = inode.Indirect % 32;

                bitmap[offset] |= (1 << index);

                std::shared_ptr<Block> indirectBlock = std::make_shared<Block>();
                disk->read(inode.Indirect, indirectBlock->Data);
                for (unsigned int Pointer : indirectBlock->Pointers) {
                    if (Pointer != 0) {
                        offset = Pointer / 32;
                        index = Pointer % 32;

                        bitmap[offset] |= (1 << index);
                    }
                }
            }
        }


    }
    // delete temp;


    return true;
}

// Create inode ----------------------------------------------------------------

ssize_t FileSystem::create() {
    // Locate free inode in inode table
    ssize_t inodeNumber = -1;
    std::shared_ptr<Block> block = std::make_shared<Block>();
    this->mountedOn->read(0, block->Data);

    for (uint32_t i = 1; i <= block->Super.InodeBlocks; i++) {
        std::shared_ptr<Block> tmp = std::make_shared<Block>();
        this->mountedOn->read(i, tmp->Data);

        for (uint32_t j = 0; j <= INODES_PER_BLOCK; j++) {
            if (tmp->Inodes[j].Valid == 0) {
                inodeNumber = (i - 1) * INODES_PER_BLOCK + j;
                tmp->Inodes[j].Valid = 1;
                break;
            }
        }

    }
    // Record inode if found
    return inodeNumber;
}

// Remove inode ----------------------------------------------------------------

bool FileSystem::remove(size_t inumber) {
    // Load inode information

    // 通过inumber直接获inode所在的inode block
    int inodeBlockIndex = inumber / INODES_PER_BLOCK + 1;
    inumber %= INODES_PER_BLOCK;
    std::shared_ptr<Block> block = std::make_shared<Block>();

    this->mountedOn->read(inodeBlockIndex, block->Data);
    auto inodeInfo = block->Inodes[inumber];
    // Free direct blocks

    //
    char non[Disk::BLOCK_SIZE];
    memset(non, 0, sizeof(non));

    for (int i = 0; i < POINTERS_PER_INODE; i++) {
        if (inodeInfo.Direct[i] != 0)
        {
            this->mountedOn->write(inodeInfo.Direct[i], non);
        }
    }
    // Free indirect blocks
    if (inodeInfo.Indirect != 0) {
        std::shared_ptr<Block> block2 = std::make_shared<Block>();
        this->mountedOn->read(inodeInfo.Indirect, block2->Data);
        for (int i = 0; i < POINTERS_PER_BLOCK; i++) {
            if (block2->Pointers[i] != 0) {
                this->mountedOn->write(block2->Pointers[i], non);
            }
        }
    }
    // Clear inode in inode table
    block->Inodes[inumber].Valid = 0;
    this->mountedOn->write(inodeBlockIndex, block->Data);
    return true;
}

// Inode stat ------------------------------------------------------------------

ssize_t FileSystem::stat(size_t inumber) {
    // Load inode information
    uint32_t inodeBlock = inumber / INODES_PER_BLOCK;
    inumber %= INODES_PER_BLOCK;
    std::shared_ptr<Block> block = std::make_share<Block>();
    this->mountedOn->read(inumber, block);


    
    return 0;
}

// Read from inode -------------------------------------------------------------

ssize_t FileSystem::read(size_t inumber, char *data, size_t length, size_t offset) {
    // Load inode information
        uint32_t inodeBlock = inumber / INODES_PER_BLOCK;
        inumber %= INODES_PER_BLOCK;
        std::shared_ptr<Block> block = std::make_share<Block>();
        this->mountedOn->read(inumber, block);
    // Adjust length

    // Read block and copy to data
    return 0;
}

// Write to inode --------------------------------------------------------------

ssize_t FileSystem::write(size_t inumber, char *data, size_t length, size_t offset) {
    // Load inode
    uint32_t inodeBlock = inumber / INODES_PER_BLOCK;
    inumber %= INODES_PER_BLOCK;
    std::shared_ptr<Block> block = std::make_share<Block>();
    this->mountedOn->read(inumber, block);
    // Write block and copy to data

    

    return 0;
}
