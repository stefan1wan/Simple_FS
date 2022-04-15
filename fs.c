#include "fs.h"
#include "disk.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>

#define FS_MAGIC           0xf0f03410
#define INODES_PER_BLOCK   128 // 64, assume 512 every block;
#define POINTERS_PER_INODE 5
#define POINTERS_PER_BLOCK 1024 //128
#define ENCODE_MASK 0xDEEDBEEF

/* don't care about config and operation, they are used mixedly*/
#define FS_CONFIG_SUCCESS 1
#define FS_CONFIG_FAIL 0
#define FS_OPERATION_SUCCESS 0
#define FS_OPERATION_FAIL -1

#define MAX(a,b) ((a) >(b)? (a):(b))

struct fs_superblock {
	int magic;
	int nblocks;
	int ninodeblocks;
	int ninodes;
	int mask;
};

struct fs_inode {
	int isvalid;
	int size;
	int direct[POINTERS_PER_INODE];
	int indirect;
};

union fs_block {
	struct fs_superblock super;
	struct fs_inode inode[INODES_PER_BLOCK];
	int pointers[POINTERS_PER_BLOCK];
	char data[DISK_BLOCK_SIZE];
};


struct fs_info{
	// pre-requisite for operations except for format and debug
	int mounted;

	// bit map
	int* free_block_bitmap; // 0 -> free
	int* free_inode_bitmap; // 0 -> free
	int free_blocks;

	// cache info in super block;
	int nblocks;
	int ninodeblocks;
	int ninodes;
} FS_INFO = {.mounted = 0};


int fs_format()
{
	// disable the mounted flag;
	FS_INFO.mounted = 0;

	union fs_block block;

	disk_read(0, block.data);
	
	int nblocks = disk_size();
	if(nblocks < 2){
		return FS_CONFIG_FAIL;
	}
	block.super.nblocks = nblocks;
	block.super.mask = ENCODE_MASK;

	// ninodeblocks =  10 percent of nblocks, rounding up.
	int ninodeblocks = (nblocks + 9)/10;
	block.super.ninodeblocks = ninodeblocks;
	block.super.ninodes = ninodeblocks * INODES_PER_BLOCK;
	disk_write(0, block.data);
	
	// set up cached general info;
	FS_INFO.nblocks = nblocks;
	FS_INFO.ninodeblocks = ninodeblocks;
	FS_INFO.ninodes = block.super.ninodes;

	// set inode block, isvalid = 0
	memset(block.data, 0x0, DISK_BLOCK_SIZE);
	// for(int i=0; i< DISK_BLOCK_SIZE; i++){
	// 	block.data[i] = rand()%256; 
	// }

	for(int i=1; i<=ninodeblocks; i++){
		disk_write(i, block.data);
	}


	return FS_CONFIG_SUCCESS;
}

void fs_debug()
{
	union fs_block block;

	disk_read(0,block.data);

	printf("superblock:\n");
	printf("    %d blocks\n",block.super.nblocks);
	printf("    %d inode blocks\n",block.super.ninodeblocks);
	printf("    %d inodes\n",block.super.ninodes);


	int ninodeblocks = block.super.ninodeblocks;
	for(int i=0; i< ninodeblocks; i++){
		disk_read(i+1, block.data);
		for(int j=0; j < INODES_PER_BLOCK; j++){
			// skip the invalid inode;
			if(!block.inode[j].isvalid) continue;

			struct fs_inode *inode = &block.inode[j]; 
			int inode_id = i*INODES_PER_BLOCK + j;
			printf("inode %d:\n", inode_id);
			printf("    size: %d bytes\n", inode->size);
			if(!inode->size) continue;

			// output the direct blocks ids
			int inode_blocks = (inode->size + DISK_BLOCK_SIZE - 1) /DISK_BLOCK_SIZE;
			int k=0;
			printf("    direct blocks: ");
			for(; k<POINTERS_PER_INODE && k<inode_blocks; k++){
				printf("%d ", inode->direct[k]);
			}
			printf("\n");

			// output indirect block id and it's inner ids;
			if(inode_blocks > POINTERS_PER_INODE && inode->indirect){
				printf("    indirect block: %d\n", inode->indirect);
				union fs_block indirect_block;
				disk_read(inode->indirect, indirect_block.data);
				printf("    indirect data blocks: ");
				for(; k<inode_blocks; k++){
					printf("%d ", indirect_block.pointers[k - POINTERS_PER_INODE]);
				}
				printf("\n");
			}
		}
	}
}

int fs_mount()
{
	if(FS_INFO.mounted)
		return FS_CONFIG_SUCCESS;

	union fs_block block;
	disk_read(0, block.data);
	int nblocks = block.super.nblocks;
	int ninodeblocks = block.super.ninodeblocks;
	int ninodes = block.super.ninodes;

	if(nblocks<2)
		return FS_CONFIG_FAIL;

	FS_INFO.free_block_bitmap = (int*)calloc(1, nblocks * sizeof(int));
	FS_INFO.free_inode_bitmap = (int*)calloc(1, ninodes * sizeof(int));
	FS_INFO.free_blocks = nblocks;
	// set super block and inode blocks not free
	for(int i=0; i< 1 + ninodeblocks; i++){
		FS_INFO.free_block_bitmap[i] = 1;
		FS_INFO.free_blocks--;
	}

	// scan the filesystem and mark;
	for(int i=0; i<ninodeblocks; i++){
		disk_read(i+1, block.data);
		for(int j=0; j < INODES_PER_BLOCK; j++){
			// skip the invalid inode;
			if(!block.inode[j].isvalid) continue;
			int inode_id = i*INODES_PER_BLOCK + j;
			FS_INFO.free_inode_bitmap[inode_id] = 1; // mark the inode;
			struct fs_inode *inode = &block.inode[j];
			if(!inode->size) continue;

			// mark the direct blocks ids
			int inode_blocks = (inode->size + DISK_BLOCK_SIZE - 1) /DISK_BLOCK_SIZE;
			int k=0;
			for(; k<POINTERS_PER_INODE && k < inode_blocks; k++)
				FS_INFO.free_block_bitmap[inode->direct[k]] = 1;
				FS_INFO.free_blocks--;

			// mark indirect block id and it's inner ids;
			if(inode_blocks > POINTERS_PER_INODE && inode->indirect){
				FS_INFO.free_block_bitmap[inode->indirect] = 1;
				FS_INFO.free_blocks--;
				union fs_block indirect_block;
				disk_read(inode->indirect, indirect_block.data);
				for(; k<inode_blocks; k++)
					FS_INFO.free_block_bitmap[indirect_block.pointers[k - POINTERS_PER_INODE]] = 1;
					FS_INFO.free_blocks--;
			}
		}
	}

	FS_INFO.mounted = 1;
	return FS_CONFIG_SUCCESS;
}

static int check_inode_number(int inumber){
	if(inumber<0 || inumber >= FS_INFO.ninodes)
		return FS_CONFIG_SUCCESS;
	return FS_CONFIG_SUCCESS;
}


static void inode_load( int inumber, struct fs_inode *inode ) { 
	int block_id = inumber/INODES_PER_BLOCK + 1;
	int inner_block_inode_id = inumber%INODES_PER_BLOCK;

	union fs_block block;
	disk_read(block_id, block.data);
	memcpy(inode, &block.inode[inner_block_inode_id], sizeof(struct fs_inode));
}

static void inode_save( int inumber, struct fs_inode *inode ) {
	int block_id = inumber/INODES_PER_BLOCK + 1;
	int inner_block_inode_id = inumber%INODES_PER_BLOCK;

	union fs_block block;
	disk_read(block_id, block.data);
	memcpy(&block.inode[inner_block_inode_id], inode, sizeof(struct fs_inode));
	disk_write(block_id, block.data);
}

int fs_create()
{
	if(!FS_INFO.mounted)
		return FS_OPERATION_FAIL;

	for(int i=0; i<FS_INFO.ninodes; i++){
		// find a free inode;
		if(FS_INFO.free_inode_bitmap[i]) 
			continue;
		
		FS_INFO.free_inode_bitmap[i] = 1;

		struct fs_inode inode;
		// if(FS_INFO.ninodes)
		inode_load(i, &inode);
		inode.isvalid = 1;
		inode.size = 0;
		inode_save(i, &inode);
		// FS_INFO.ninodes ++;

		return i; // the inode number;
	}
	return FS_OPERATION_FAIL;
}


int fs_delete( int inumber )
{
	if(!FS_INFO.mounted)
		return FS_CONFIG_FAIL;

	if(!check_inode_number(inumber))
		return FS_CONFIG_FAIL;
	
	if(!FS_INFO.free_inode_bitmap[inumber])
		return FS_CONFIG_FAIL;

	// set inode mark;
	FS_INFO.free_inode_bitmap[inumber]	= 0;
	
	struct fs_inode inode; 
	inode_load(inumber, &inode);
	inode.isvalid = 0;
	int inode_blocks = (inode.size + DISK_BLOCK_SIZE - 1) /DISK_BLOCK_SIZE;
	int k=0;
	// free direct blocks
	for(;k<inode_blocks && k<POINTERS_PER_INODE; k++){
		// mark block free
		FS_INFO.free_block_bitmap[inode.direct[k]] = 0;
		FS_INFO.free_blocks++;
		// set direct block 0
		inode.direct[k] = 0;
	}

	// free indirect block id and it's inner blocks;
	if(inode_blocks > POINTERS_PER_INODE && inode.indirect){
		union fs_block indirect_block;
		disk_read(inode.indirect, indirect_block.data);

		for(; k<inode_blocks; k++){
			FS_INFO.free_block_bitmap[indirect_block.pointers[k - POINTERS_PER_INODE]] = 0;
			FS_INFO.free_blocks++;
		}

		// mark indirect block free
		FS_INFO.free_block_bitmap[inode.indirect] = 0;
		FS_INFO.free_blocks++;
		// set indrect block 0;
		inode.indirect = 0;
	}
	inode.size = 0;
	inode_save(inumber, &inode);
	return FS_CONFIG_SUCCESS;
}

int fs_getsize( int inumber )
{
	if(!FS_INFO.mounted)
		return FS_OPERATION_FAIL;

	if(!check_inode_number(inumber))
		return FS_OPERATION_FAIL;
	
	if(!FS_INFO.free_inode_bitmap[inumber])
		return FS_OPERATION_FAIL;

	struct fs_inode inode; 
	inode_load(inumber, &inode);
	return inode.size;
}

int fs_getmask(){
	if(!FS_INFO.mounted)
		return FS_OPERATION_FAIL;

	union fs_block block;

	disk_read(0,block.data);

	return block.super.mask;
}

static int translate_block(struct fs_inode* inode, int inner_block_id){
	if(inner_block_id<POINTERS_PER_INODE)
		return inode->direct[inner_block_id];

	assert(inode->indirect!=0);
	// printf("DEBUG: inode->indirect: %d\n", inode->indirect);
	union fs_block block;
	disk_read(inode->indirect, block.data);
	return block.pointers[inner_block_id - POINTERS_PER_INODE];
}

int fs_read( int inumber, char *data, int length, int offset )
{
	if(!FS_INFO.mounted)
		return FS_CONFIG_FAIL;

	if(!check_inode_number(inumber))
		return FS_CONFIG_FAIL;
	
	if(!FS_INFO.free_inode_bitmap[inumber])
		return FS_CONFIG_FAIL;

	if(length<=0)
		return 0;

	struct fs_inode inode; 
	inode_load(inumber, &inode);	

	int start = offset;
	int end = offset + length > inode.size? inode.size: offset + length;

	int start_block = start / DISK_BLOCK_SIZE;
	int start_offset = start % DISK_BLOCK_SIZE;
	int end_block = end  / DISK_BLOCK_SIZE;
	int end_offset = end % DISK_BLOCK_SIZE;

	union fs_block block;
	if(start_block == end_block){
		
		disk_read(translate_block(&inode, start_block), block.data);
		memcpy(data, block.data + start_offset, end_offset - start_offset);
		return end_offset - start_offset;
	}

	int read_len = 0;
	// read start block
	if(start_offset!=0){
		disk_read(translate_block(&inode, start_block), block.data);
		memcpy(data,  block.data + start_offset, DISK_BLOCK_SIZE - start_offset);
		read_len += DISK_BLOCK_SIZE - start_offset;
	}else{
		disk_read(translate_block(&inode, start_block), data);
		read_len += DISK_BLOCK_SIZE;
	}

	// read inner blocks
	for(int i = start_block + 1; i< end_block; i++){
		disk_read(translate_block(&inode, i), data + read_len);
		read_len += DISK_BLOCK_SIZE;
	}

	// read end block
	disk_read(translate_block(&inode, end_block), block.data);
	memcpy(data + read_len,  block.data, end_offset);
	read_len += end_offset;
	return read_len;
	

}

static int find_free_block(){
	for(int i=0; i<FS_INFO.nblocks; i++)
		if(!FS_INFO.free_block_bitmap[i])
			return i;
	return 0;
}

int fs_write( int inumber, const char *data, int length, int offset )
{
	if(!FS_INFO.mounted)
		return FS_CONFIG_FAIL;

	if(!check_inode_number(inumber))
		return FS_CONFIG_FAIL;
	
	if(!FS_INFO.free_inode_bitmap[inumber])
		return FS_CONFIG_FAIL;

	if(length<=0)
		return 0;

	struct fs_inode inode; 
	inode_load(inumber, &inode);

	int start = offset;
	int end = offset + length;
	// int current_end = inode.size;

	// they are numbers not IDs
	int exists_block = (inode.size + DISK_BLOCK_SIZE - 1) /DISK_BLOCK_SIZE; 
	int all_blocks = (MAX(end, inode.size) + DISK_BLOCK_SIZE - 1) /DISK_BLOCK_SIZE;

	int needed = all_blocks - exists_block;

	if(all_blocks > POINTERS_PER_INODE + POINTERS_PER_BLOCK)
		return FS_CONFIG_FAIL;
	
	if(needed > FS_INFO.free_blocks)
		return FS_CONFIG_FAIL;
	// printf("needed:%d, free_blocks:%d\n", needed, FS_INFO.free_blocks);

	/* 
	* get the blocks once a time 
	* and save them to inodes
	* save the inodes;
	*/
	if(all_blocks > exists_block){
		// create the indirect block if necessory;
		union fs_block* indirect_block = (union fs_block*) calloc(1, sizeof(union fs_block));

		if(exists_block<=POINTERS_PER_INODE && all_blocks > POINTERS_PER_INODE){
			int block_id = find_free_block();
			assert(block_id!=0);
			FS_INFO.free_block_bitmap[block_id] = 1;
			FS_INFO.free_blocks--;
			inode.indirect = block_id;
		}else if(all_blocks > POINTERS_PER_INODE){
			disk_read(inode.indirect, indirect_block->data);
		}

		

		// find free blocks;
		for(int i=0; i<needed; i++){
			int block_id = find_free_block();
			assert(block_id!=0);
			FS_INFO.free_block_bitmap[block_id] = 1;
			FS_INFO.free_blocks--;
			// new_blocks[new_blocks_p++] = block_id;
			int inner_block_id = exists_block + i;
			if(inner_block_id < POINTERS_PER_INODE){
				inode.direct[inner_block_id] = block_id;
			}else{
				indirect_block->pointers[inner_block_id - POINTERS_PER_INODE] = block_id;
			}
		}

		if(all_blocks > POINTERS_PER_INODE)
			disk_write(inode.indirect, indirect_block->data);

		inode.size = end; // end must lager than original size
		inode_save(inumber, &inode);
	}

	int start_block = start / DISK_BLOCK_SIZE;
	int start_offset = start % DISK_BLOCK_SIZE;
	int end_block = end  / DISK_BLOCK_SIZE;
	int end_offset = end % DISK_BLOCK_SIZE;

	union fs_block block;
	/* if one the same block*/
	if(start_block == end_block){
		int block_id = translate_block(&inode, start_block);
		disk_read(block_id, block.data);
		memcpy(block.data + start_offset, data, end_offset - start_offset);
		disk_write(block_id, block.data);
		return end_offset - start_offset;
	}

	int write_len = 0;

	/* write the first block */
	if(start_offset!=0){
		int block_id = translate_block(&inode, start_block);
		disk_read(block_id, block.data);
		memcpy(block.data + start_offset, data, DISK_BLOCK_SIZE - start_offset);
		disk_write(block_id, block.data);
		write_len += DISK_BLOCK_SIZE - start_offset;
	}else{
		int block_id = translate_block(&inode, start_block);
		disk_write(block_id, data);
		write_len += DISK_BLOCK_SIZE;
	}
	
	/* write until all the current blocks runs out */
	int i;
	for(i = start_block + 1; i<=end_block; i++){
		int block_id = translate_block(&inode, i);
		if(i== end_block && end_offset != 0){ // end_offset == 0 means it's the end 
			disk_read(block_id, block.data);
			memcpy(block.data, data + write_len, end_offset);
			disk_write(block_id, block.data);
			write_len += end_offset;
		}else if(i < end_block) {
			disk_write(block_id, data + write_len);
			write_len += DISK_BLOCK_SIZE;
		}
	}

	return write_len;
}
