/*
 *  linux/kernel/blk_dev/ll_rw.c
 *
 * (C) 1991 Linus Torvalds
 */

/*
 * This handles all read/write requests to block devices
 */
#include <errno.h>
#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/system.h>

#include "blk.h"

/*
 * The request-struct contains all necessary data
 * to load a nr of sectors into memory
 */
struct request request[NR_REQUEST];

/*
 * used to wait on when there are no free requests
 */
struct task_struct * wait_for_request = NULL;

/* blk_dev_struct is:
 *	do_request-address
 *	next-request
 */
 // blk_dev_struct 对应一个函数回调，和一个具体的请求（读或者写）
struct blk_dev_struct blk_dev[NR_BLK_DEV] = {
	{ NULL, NULL },		/* no_dev */
	{ NULL, NULL },		/* dev mem */
	{ NULL, NULL },		/* dev fd */
	{ NULL, NULL },		/* dev hd */
	{ NULL, NULL },		/* dev ttyx */
	{ NULL, NULL },		/* dev tty */
	{ NULL, NULL }		/* dev lp */
};

static inline void lock_buffer(struct buffer_head * bh)
{
	cli();

	// 如果当前的bf的锁被占用，那就先切换进程，直到锁被释放了。
	while (bh->b_lock)
		sleep_on(&bh->b_wait);

	// 上锁
	bh->b_lock=1;	
	sti();
}

static inline void unlock_buffer(struct buffer_head * bh)
{
	if (!bh->b_lock)
		printk("ll_rw_block.c: buffer not locked\n\r");
	bh->b_lock = 0;
	wake_up(&bh->b_wait);
}

/*
 * add-request adds a request to the linked list.
 * It disables interrupts so that it can muck with the
 * request-lists in peace.
 */
static void add_request(struct blk_dev_struct * dev, struct request * req)
{
	struct request * tmp;

	req->next = NULL;

	// 上锁，也就是关闭中断
	cli();

	
	if (req->bh)

		// 把当前请求的缓存头设置为不脏了，因为马上要被做掉了。
		req->bh->b_dirt = 0;

	// 如果当前处理硬盘       中没有请求就把当前的请求给他处理。
	if (!(tmp = dev->current_request)) {

		// 赋值请求.
		dev->current_request = req;

		// 释放锁
		sti();

		// 处理函数指针。所以真真的处理逻辑就在这里.
		(dev->request_fn)();
		return;
	}
	
	// 能到这里来就说明，当前dev设备，比如硬盘已经有请求了。
	// 因为request请求是一个单链表。
	// 做比较。
	// 第一次tmp是当前正常处理的请求，之后tmp是当前正在处理请求链表的下一个，   req是当前进来的请求.
	// 目的是为了找到链表的插入位置。
	for ( ; tmp->next ; tmp=tmp->next)	
		if ((IN_ORDER(tmp,req) ||
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;

	// 添加到链表尾部的操作。
	req->next=tmp->next;
	tmp->next=req;
	sti();
}

static void make_request(int major,int rw, struct buffer_head * bh)
{
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	// 预读写的逻辑，暂时不去出处理。
	if (rw_ahead = (rw == READA || rw == WRITEA)) {

		// 被锁了直接溜了
		if (bh->b_lock)
			return;
		
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}

	// 不是读，不是写，那是怪物？
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");

	// 先看已经被上锁没，被上锁了就等待释放锁，再获取锁。
	// 准备动手，先上锁。不上锁的话，等等一边写高速缓存，一边高速缓存落盘磁盘，那就G了
	lock_buffer(bh);

	// 如果在等待的过程中，别的进程已经把缓存区的资源落盘了，或者已经把读的数据读完了，那不贼爽，我啥都不同干了。
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
	
		// 进到这里表示，读写的任务被别人干完了，我只需要把我当前上的锁给释放就行。
		// 释放当前缓存头的锁，并且将当前缓存头中等待队列唤醒。
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
 	// 走到这里表示锁还没释放，所以这边就是处理请求的具体逻辑
 	// 如果是读      就获取到请求数组的最后一位地址
	if (rw == READ)

		// 获取到请求数组的最后一位元素的地址。
		req = request+NR_REQUEST;

	// 如果是写就获取到接近2/3的位置。因为要预留一小块位置给读。因为读牛逼...
	else
		req = request+((NR_REQUEST*2)/3);
	
/* find an empty request */
	// 从读或者写的最后一位开始遍历。 直到找到空闲的。
	while (--req >= request)

		// dev为-1代表没被使用.
		if (req->dev<0)
			break;
		
/* if none found, sleep on new requests: check for rw_ahead */
	// 如果找到了就代表req不为首地址
	// 如果没有找到就代表req为首地址 - 一个request结构体的地址。
	// 所以这里是没有找到。
	if (req < request) {

		// 如果预读或者预写了就释放锁直接跑，预读写这块暂时不懂，不过不影响整体。
		if (rw_ahead) {

			// 释放锁，并且唤醒被睡眠的进程，让他们醒来判断锁。
			unlock_buffer(bh);
			return;
		}

		// 因为在request数组中没找到空闲的  先休息一下，然后从头再来。
		sleep_on(&wait_for_request);
		goto repeat;
	}

	// 走到这里代表找到了，然后占坑。
/* fill up the request-info, and add it to the queue */
	req->dev = bh->b_dev;
	req->cmd = rw;
	req->errors=0;
	req->sector = bh->b_blocknr<<1;
	req->nr_sectors = 2;
	req->buffer = bh->b_data;
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;

	// 添加到请求队列中，并且处理的逻辑。
	add_request(major+blk_dev,req);
}

// 把高速缓存落盘的实际handler
void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;

	// 
	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	!(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}

	
	make_request(major,rw,bh);
}

void blk_dev_init(void)
{
	int i;

	for (i=0 ; i<NR_REQUEST ; i++) {
		request[i].dev = -1;
		request[i].next = NULL;
	}
}
