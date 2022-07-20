/*
 *  linux/fs/block_dev.c
 *
 *  (C) 1991  Linus Torvalds
 */

#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

int block_write(int dev, long * pos, char * buf, int count)
{
	// 得到当前在第几块
	int block = *pos >> BLOCK_SIZE_BITS;

	// 得到具体的偏移量
	int offset = *pos & (BLOCK_SIZE-1);
	
	int chars;
	
	int written = 0;
	
	struct buffer_head * bh;
	
	register char * p;
	
	while (count>0) {

		// 一个块的空间减去偏移量，等于当前这个块剩余的数量
		chars = BLOCK_SIZE - offset;
		
		if (chars > count)
			chars=count;
		if (chars == BLOCK_SIZE)

			// 根据设备号和第几块得到具体的缓存头。
			// 内部维护了一个hash表+链表。   空闲链表。
			bh = getblk(dev,block);
		else
			bh = breada(dev,block,block+1,block+2,-1);
		
		block++;
		if (!bh)
			return written?written:-EIO;

		// 得到缓存块操作的具体位置。
		p = offset + bh->b_data;
		
		offset = 0;
		
		*pos += chars;
		
		written += chars;
		
		count -= chars;
		
		while (chars-->0)
			*(p++) = get_fs_byte(buf++);
		bh->b_dirt = 1;
		brelse(bh);
	}
	return written;
}

int block_read(int dev, unsigned long * pos, char * buf, int count)
{
	int block = *pos >> BLOCK_SIZE_BITS;
	int offset = *pos & (BLOCK_SIZE-1);
	int chars;
	int read = 0;
	struct buffer_head * bh;
	register char * p;

	while (count>0) {
		chars = BLOCK_SIZE-offset;
		if (chars > count)
			chars = count;
		if (!(bh = breada(dev,block,block+1,block+2,-1)))
			return read?read:-EIO;
		block++;
		p = offset + bh->b_data;
		offset = 0;
		*pos += chars;
		read += chars;
		count -= chars;
		while (chars-->0)
			put_fs_byte(*(p++),buf++);
		brelse(bh);
	}
	return read;
}
