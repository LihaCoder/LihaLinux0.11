/*
 *  linux/mm/memory.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * demand-loading started 01.12.91 - seems it is high on the list of
 * things wanted, and it should be easy to implement. - Linus
 */

/*
 * Ok, demand-loading was easy, shared pages a little bit tricker. Shared
 * pages started 02.12.91, seems to work. - Linus.
 *
 * Tested sharing by executing about 30 /bin/sh: under the old kernel it
 * would have taken more than the 6M I have free, but it worked well as
 * far as I could see.
 *
 * Also corrected some "invalidate()"s - I wasn't doing enough of them.
 */

#include <signal.h>

#include <asm/system.h>

#include <linux/sched.h>
#include <linux/head.h>
#include <linux/kernel.h>

volatile void do_exit(long code);

static inline volatile void oom(void)
{
	printk("out of memory\n\r");
	do_exit(SIGSEGV);
}

#define invalidate() \
__asm__("movl %%eax,%%cr3"::"a" (0))

/* these are not to be changed without changing head.s etc */
// 内核占用的大小
#define LOW_MEM 0x100000

// 因为内核占用了1MB,所以这里只有15MB的物理内存
#define PAGING_MEMORY (15*1024*1024)

// 总共的页表，因为内存只有15MB，而一个页占用4kb大小，所以15MB/4KB = 3840个页
#define PAGING_PAGES (PAGING_MEMORY>>12)
#define MAP_NR(addr) (((addr)-LOW_MEM)>>12)
#define USED 100

#define CODE_SPACE(addr) ((((addr)+4095)&~4095) < \
current->start_code + current->end_code)

static long HIGH_MEMORY = 0;

#define copy_page(from,to) \
__asm__("cld ; rep ; movsl"::"S" (from),"D" (to),"c" (1024):"cx","di","si")

// 0代表空闲，非0代表已经被使用
static unsigned char mem_map [ PAGING_PAGES ] = {0,};

/*
 * Get physical address of first (actually last :-) free page, and mark it
 * used. If no free pages left, return 0.
 */
unsigned long get_free_page(void)
{
register unsigned long __res asm("ax");

__asm__("std ; repne ; scasb\n\t"	// 这里就是在遍历mem_map数组，然后与eax的值做比较。repne指令用ecx寄存器来做计数器循环，scasb指令比较eax和di的值
	"jne 1f\n\t"					// 如果遍历完mem_map数组没有得到一个为0的就直接歇逼，代表所有数组被使用
	"movb $1,1(%%edi)\n\t"			// 执行到这里代表找到mem_map为0的下标，也就是找到了空白页。这一步目的就是占坑
	"sall $12,%%ecx\n\t"			// 所以这里的ecx寄存器的值是上面计数器自减后的值，也就是找到空白页的索引，左移12位后得到地址
	"addl %2,%%ecx\n\t"				// 这里要加上内核使用的1mb的基址，得到绝对地址
	"movl %%ecx,%%edx\n\t"			// 把ecx中绝对地址页表的地址给edx
	"movl $1024,%%ecx\n\t"			// 1024给ecx
	"leal 4092(%%edx),%%edi\n\t"	// 一个页表是4096的大小，而对于页表来说一个表项是4byte，所以这是得到空闲页表的最后一个元素地址
	"rep ; stosl\n\t"				// 
	"movl %%edx,%%eax\n"			// 这里的edx是空闲页表的基址给eax返回
	"1:"
	:"=a" (__res)
	:"0" (0),"i" (LOW_MEM),"c" (PAGING_PAGES),
	"D" (mem_map+PAGING_PAGES-1)
	:"di","cx","dx");
return __res;
}

/*
 * Free a page of memory at physical address 'addr'. Used by
 * 'free_page_tables()'
 */
void free_page(unsigned long addr)
{
	if (addr < LOW_MEM) return;
	if (addr >= HIGH_MEMORY)
		panic("trying to free nonexistent page");
	addr -= LOW_MEM;
	addr >>= 12;
	if (mem_map[addr]--) return;
	mem_map[addr]=0;
	panic("trying to free free page");
}

/*
 * This function frees a continuos block of page tables, as needed
 * by 'exit()'. As does copy_page_tables(), this handles only 4Mb blocks.
 */
int free_page_tables(unsigned long from,unsigned long size)
{
	unsigned long *pg_table;
	unsigned long * dir, nr;

	if (from & 0x3fffff)
		panic("free_page_tables called with wrong alignment");
	if (!from)
		panic("Trying to free up swapper memory space");
	size = (size + 0x3fffff) >> 22;
	dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */
	for ( ; size-->0 ; dir++) {
		if (!(1 & *dir))
			continue;
		pg_table = (unsigned long *) (0xfffff000 & *dir);
		for (nr=0 ; nr<1024 ; nr++) {
			if (1 & *pg_table)
				free_page(0xfffff000 & *pg_table);
			*pg_table = 0;
			pg_table++;
		}
		free_page(0xfffff000 & *dir);
		*dir = 0;
	}
	invalidate();
	return 0;
}

/*
 *  Well, here is one of the most complicated functions in mm. It
 * copies a range of linerar addresses by copying only the pages.
 * Let's hope this is bug-free, 'cause this one I don't want to debug :-)
 *
 * Note! We don't copy just any chunks of memory - addresses have to
 * be divisible by 4Mb (one page-directory entry), as this makes the
 * function easier. It's used only by fork anyway.
 *
 * NOTE 2!! When from==0 we are copying kernel space for the first
 * fork(). Then we DONT want to copy a full page-directory entry, as
 * that would lead to some serious memory waste - we just copy the
 * first 160 pages - 640kB. Even that is more than we need, but it
 * doesn't take any more memory - we don't copy-on-write in the low
 * 1 Mb-range, so the pages can be shared with the kernel. Thus the
 * special case for nr=xxxx.
 */
int copy_page_tables(unsigned long from,unsigned long to,long size)
{
	unsigned long * from_page_table;	// 父进程  页目录表项指向页表的地址
	unsigned long * to_page_table;		// 子进程     页目录表项指向页表的地址
	unsigned long this_page;
	unsigned long * from_dir, * to_dir;
	unsigned long nr;

	if ((from&0x3fffff) || (to&0x3fffff))
		panic("copy_page_tables called with wrong alignment");

	// 获取到页目录的偏移量
	from_dir = (unsigned long *) ((from>>20) & 0xffc); /* _pg_dir = 0 */

	
	// 获取到页目录的偏移量
	to_dir = (unsigned long *) ((to>>20) & 0xffc);

	
	size = ((unsigned) (size+0x3fffff)) >> 22;
	for( ; size-->0 ; from_dir++,to_dir++) {

		// 只有*to_dir的最后一位为0才不执行这里
		// 最后一位是状态p，0表示不可用，1表示可用
		// 1表示当前页表已经存在
		if (1 & *to_dir)
			panic("copy_page_tables: already exist");

		// *from_dir(页目录表项PDE)最后一位为0，整个就为0了，骚的不行
		// 最后一位是状态p，0表示不可用，1表示可用
		if (!(1 & *from_dir))
			continue;


		// 这里很骚
		// from_dir偏移量就是页目录表项（PDE）的地址，因为在head.s中初始化页目录从0开始
		// 而这里0xfffff000 & 的操作是为了将最后12位置为0，反客为主？
		// 为什么不*from_dir & 0xfffff000嗯....  好像是一样的结果
		// 目的是为了将PDE中的最后12位清零，目的是为了得到一个完整的20位的页表基地址
		from_page_table = (unsigned long *) (0xfffff000 & *from_dir);

		// 这里寻找一个空闲的页表，也就是为子进程找页表
		// 这里是找到空闲页表的基址
		if (!(to_page_table = (unsigned long *) get_free_page()))
			return -1;	/* Out of memory, see freeing */

		// 这里把地址转成long，其实也就是为了能 或7
		// | 操作是二进制组合 7 = 0111;
		// 地址 | 7？
		*to_dir = ((unsigned long) to_page_table) | 7;

		// 不为0  所以nr = 1024
		// 这是在遍历一个页表  ，1024个页表项
		nr = (from==0)?0xA0:1024;

		// 这边其实就是在遍历这个页表所有项，一个页表是1024*4byte = 4kb
		// from_page_table++是操作页表,因为地址+1代表你懂得
		// to_page_table++是操作页表,因为地址+1代表你懂得
		for ( ; nr-- > 0 ; from_page_table++,to_page_table++) {

			// from_page_table 是页目录表项指向的页表
			// *from_page_table解引用，这里解引用后还是一个4byte，32位的描述符
			// this_page 也就是页表（理解成一个数组，指向最低）
			// 这里是获取到具体的PTE
			this_page = *from_page_table;	

			// this_page最后一位为0，整个就为0了，骚的不行
			// 最后一位是状态p，0表示不可用，1表示可用
			if (!(1 & this_page))	
				continue;

			// 十进制2 = 0010 进行取反和与操作后将this_page的第2位置为0 也就是只读
			this_page &= ~2;   

			// 这不就是在复制了么... 嘻嘻             闭环  
			// 所以最后子进程和父进程的页都是只读的
			*to_page_table = this_page;


			// LOW_MEM宏定义，为0x100000  为1MB
			if (this_page > LOW_MEM) {
				*from_page_table = this_page;
				this_page -= LOW_MEM;
				this_page >>= 12;
				mem_map[this_page]++;
			}
		}
	}
	invalidate();
	return 0;
}

/*
 * This function puts a page in memory at the wanted address.
 * It returns the physical address of the page gotten, 0 if
 * out of memory (either when trying to access page-table or
 * page.)
 	这个方法放置一个页在内存中所需的地址
 	返回值是物理地址，如果返回为0就是oom了
 */
 // page 是找到的空闲页的基址
 // address 是出现页异常的线性地址
unsigned long put_page(unsigned long page,unsigned long address)
{
	unsigned long tmp, *page_table;

/* NOTE !!! This uses the fact that _pg_dir=0 */

	if (page < LOW_MEM || page >= HIGH_MEMORY)
		printk("Trying to put page %p at %p\n",page,address);

	// mem_map是一个数组，为0代表没使用，为1代表已经被占坑
	// mem_map[(page-LOW_MEM)>>12 得到页对应的数组下表
	// 为1代表占坑了，不为1就代表有问题
	if (mem_map[(page-LOW_MEM)>>12] != 1)
		printk("mem_map disagrees with %p at %p\n",page,address);

	// address>>20 & 0xffc 得到页目录的偏移量
	page_table = (unsigned long *) ((address>>20) & 0xffc);

	// 偏移量就是实际地址，因为页目录是从0开始的。
	// *page_table得到页目录的表项，如果最后一位为0就走else，如果不为0就走if
	// 最后一位为1代表可用，为0代表不可用。
	if ((*page_table)&1)

		// 从PDE描述符中得到高20位地址，也就是页表的基地址
		// 最终把页表的基址给page_table
		page_table = (unsigned long *) (0xfffff000 & *page_table);
	else {
		
		// 这里再做一次争取，梦回AQS？
		if (!(tmp=get_free_page()))
			return 0;
		*page_table = tmp|7;
		page_table = (unsigned long *) tmp;
	}
	page_table[(address>>12) & 0x3ff] = page | 7;
/* no need for invalidate */
	return page;
}

void un_wp_page(unsigned long * table_entry)
{
	unsigned long old_page,new_page;

	old_page = 0xfffff000 & *table_entry;
	if (old_page >= LOW_MEM && mem_map[MAP_NR(old_page)]==1) {
		*table_entry |= 2;
		invalidate();
		return;
	}
	if (!(new_page=get_free_page()))
		oom();
	if (old_page >= LOW_MEM)
		mem_map[MAP_NR(old_page)]--;
	*table_entry = new_page | 7;
	invalidate();
	copy_page(old_page,new_page);
}	

/*
 * This routine handles present pages, when users try to write
 * to a shared page. It is done by copying the page to a new address
 * and decrementing the shared-page counter for the old page.
 *
 * If it's in code space we exit with a segment error.
 */
void do_wp_page(unsigned long error_code,unsigned long address)
{
#if 0
/* we cannot do this yet: the estdio library writes to code space */
/* stupid, stupid. I really want the libc.a from GNU */
	if (CODE_SPACE(address))
		do_exit(SIGSEGV);
#endif
	un_wp_page((unsigned long *)
		(((address>>10) & 0xffc) + (0xfffff000 &
		*((unsigned long *) ((address>>20) &0xffc)))));

}

void write_verify(unsigned long address)
{
	unsigned long page;

	if (!( (page = *((unsigned long *) ((address>>20) & 0xffc)) )&1))
		return;
	page &= 0xfffff000;
	page += ((address>>10) & 0xffc);
	if ((3 & *(unsigned long *) page) == 1)  /* non-writeable, present */
		un_wp_page((unsigned long *) page);
	return;
}

// address为线性地址
void get_empty_page(unsigned long address)
{
	unsigned long tmp;

	// get_free_page 只是找到一个空闲也的地址，如果没内存了（也就是页用完了）就返回0
	// put_page将地址的空闲也开辟出来             什么是开辟出来？   我的理解就是把线性地址做映射
	if (!(tmp=get_free_page()) || !put_page(tmp,address)) {
		free_page(tmp);		/* 0 is ok - ignored */
		oom();
	}
}

/*
 * try_to_share() checks the page at address "address" in the task "p",
 * to see if it exists, and if it is clean. If so, share it with the current
 * task.
 *
 * NOTE! This assumes we have checked that p != current, and that they
 * share the same executable.
 */
static int try_to_share(unsigned long address, struct task_struct * p)
{
	unsigned long from;
	unsigned long to;
	unsigned long from_page;
	unsigned long to_page;
	unsigned long phys_addr;

	from_page = to_page = ((address>>20) & 0xffc);
	from_page += ((p->start_code>>20) & 0xffc);
	to_page += ((current->start_code>>20) & 0xffc);
/* is there a page-directory at from? */
	from = *(unsigned long *) from_page;
	if (!(from & 1))
		return 0;
	from &= 0xfffff000;
	from_page = from + ((address>>10) & 0xffc);
	phys_addr = *(unsigned long *) from_page;
/* is the page clean and present? */
	if ((phys_addr & 0x41) != 0x01)
		return 0;
	phys_addr &= 0xfffff000;
	if (phys_addr >= HIGH_MEMORY || phys_addr < LOW_MEM)
		return 0;
	to = *(unsigned long *) to_page;
	if (!(to & 1))
		if (to = get_free_page())
			*(unsigned long *) to_page = to | 7;
		else
			oom();
	to &= 0xfffff000;
	to_page = to + ((address>>10) & 0xffc);
	if (1 & *(unsigned long *) to_page)
		panic("try_to_share: to_page already exists");
/* share them: write-protect */
	*(unsigned long *) from_page &= ~2;
	*(unsigned long *) to_page = *(unsigned long *) from_page;
	invalidate();
	phys_addr -= LOW_MEM;
	phys_addr >>= 12;
	mem_map[phys_addr]++;
	return 1;
}

/*
 * share_page() tries to find a process that could share a page with
 * the current one. Address is the address of the wanted page relative
 * to the current data space.
 *
 * We first check if it is at all feasible by checking executable->i_count.
 * It should be >1 if there are other tasks sharing this inode.
 */
static int share_page(unsigned long address)
{
	struct task_struct ** p;

	if (!current->executable)
		return 0;
	if (current->executable->i_count < 2)
		return 0;
	for (p = &LAST_TASK ; p > &FIRST_TASK ; --p) {
		if (!*p)
			continue;
		if (current == *p)
			continue;
		if ((*p)->executable != current->executable)
			continue;
		if (try_to_share(address,*p))
			return 1;
	}
	return 0;
}


/*
	缺页处理
	unsigned long error_code:为错误码
	unsigned long address:为因为缺页异常的线性地址
*/
void do_no_page(unsigned long error_code,unsigned long address)
{
	int nr[4];
	unsigned long tmp;
	unsigned long page;
	int block,i;

	// 因为是线性地址  所以10(页目录偏移量) 10(页表偏移量) 12(页帧偏移量)
	// 截断后12位地址，得到高20位地址
	address &= 0xfffff000;

	// current->start_code 为当前代码段的地址
	// 当前出现缺页错误的线性地址减去线性地址中最低的地址得到
	tmp = address - current->start_code;

	// current->executable 为执行文件i节点结构
	// current->end_data 为代码长度 + 数据长度
	// tmp >= current->end_data 也就是tmp的值超过了代码长度 + 数据长度，所以需要开辟一个物理页，并且把开辟的页的信息存放在address（线性地址）中
	// current->executable如果为0就代表进程刚开始设置，需要内存，所以开辟一个物理页，并且把开辟的页的信息存放在address（线性地址）中
	if (!current->executable || tmp >= current->end_data) {

		// 申请一个物理页，底层还是通过get_free_page()方法来实现。
		get_empty_page(address);
		return;
	}

	// 尝试共享，但是内部看不懂
	if (share_page(tmp))
		return;

	// 找到一个空白的页表，如果返回为0就代表没找到
	if (!(page = get_free_page()))

		// 直接退出当前进程，切换另外可执行的进程
		oom();
	
/* remember that 1 block is used for header */
	// BLOCK_SIZE = 1024
	block = 1 + tmp/BLOCK_SIZE;
	
	for (i=0 ; i<4 ; block++,i++)
		nr[i] = bmap(current->executable,block);
	
	bread_page(page,current->executable->i_dev,nr);
	
	i = tmp + 4096 - current->end_data;

	// 下一个页表？
	tmp = page + 4096;
	
	while (i-- > 0) {
		tmp--;
		*(char *)tmp = 0;
	}
	
	if (put_page(page,address))
		return;
	free_page(page);
	oom();
}

void mem_init(long start_mem, long end_mem)
{
	int i;

	HIGH_MEMORY = end_mem;
	for (i=0 ; i<PAGING_PAGES ; i++)
		mem_map[i] = USED;
	i = MAP_NR(start_mem);
	end_mem -= start_mem;
	end_mem >>= 12;
	while (end_mem-->0)
		mem_map[i++]=0;
}

void calc_mem(void)
{
	int i,j,k,free=0;
	long * pg_tbl;

	for(i=0 ; i<PAGING_PAGES ; i++)
		if (!mem_map[i]) free++;
	printk("%d pages free (of %d)\n\r",free,PAGING_PAGES);
	for(i=2 ; i<1024 ; i++) {
		if (1&pg_dir[i]) {
			pg_tbl=(long *) (0xfffff000 & pg_dir[i]);
			for(j=k=0 ; j<1024 ; j++)
				if (pg_tbl[j]&1)
					k++;
			printk("Pg-dir[%d] uses %d pages\n",i,k);
		}
	}
}
