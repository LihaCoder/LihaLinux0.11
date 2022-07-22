/*
 *  linux/kernel/sched.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * 'sched.c' is the main kernel file. It contains scheduling primitives
 * (sleep_on, wakeup, schedule etc) as well as a number of simple system
 * call functions (type getpid(), which just extracts a field from
 * current-task
 */
#include <linux/sched.h>
#include <linux/kernel.h>
#include <linux/sys.h>
#include <linux/fdreg.h>
#include <asm/system.h>
#include <asm/io.h>
#include <asm/segment.h>

#include <signal.h>

// SIGKILL = 9    1<< (9-1) = 256 = 0001 0000 0000
// SIGSTOP = 19   1<< (19-1) = 262,144 = 0100 0000 0000 0000 0000
// |操作后值为:0100 0000 0001 0000 0000
// ~操作后值为:1011 1111 1110 1111 1111 = 0xBFEFF = 786175
#define _S(nr) (1<<((nr)-1))
#define _BLOCKABLE (~(_S(SIGKILL) | _S(SIGSTOP)))

void show_task(int nr,struct task_struct * p)
{
	int i,j = 4096-sizeof(struct task_struct);

	printk("%d: pid=%d, state=%d, ",nr,p->pid,p->state);
	i=0;
	while (i<j && !((char *)(p+1))[i])
		i++;
	printk("%d (of %d) chars free in kernel stack\n\r",i,j);
}

void show_stat(void)
{
	int i;

	for (i=0;i<NR_TASKS;i++)
		if (task[i])
			show_task(i,task[i]);
}

#define LATCH (1193180/HZ)

extern void mem_use(void);

extern int timer_interrupt(void);
extern int system_call(void);

// c语言中union关键字的作用如下：
// 1.当存在多个元素时，只会开辟其中最大空间大小的元素的大小。
// 2.同时只能解释成员中的一位。所以要合理使用。
// 所以这里开辟了4096byte大小。
union task_union {
	struct task_struct task;
	char stack[PAGE_SIZE];		// 大小为4096=4kb     也就是一个页表的大小。
};

static union task_union init_task = {INIT_TASK,};

// 从开机开始记录的时钟滴答的次数，频率为10ms一次
// 用途特别的广泛
long volatile jiffies=0;
long startup_time=0;

// *current结构体指针为正在执行的进程，默认为init_task也就是0号进程
struct task_struct *current = &(init_task.task);
struct task_struct *last_task_used_math = NULL;

// 首先指针的用处是什么？		       用当前指针的类型解释这片内存
// 所以这边就是用task_struct结构体来解释一个长度为64的数组
// 并且当前数组的首地址（0号下标）已经放入了init_task.task
// 所以init_task.task也就是0号task
struct task_struct * task[NR_TASKS] = {&(init_task.task), };

long user_stack [ PAGE_SIZE>>2 ] ;

struct {
	long * a;
	short b;
	} stack_start = { & user_stack [PAGE_SIZE>>2] , 0x10 };
/*
 *  'math_state_restore()' saves the current math information in the
 * old math state array, and gets the new ones from the current task
 */
void math_state_restore()
{
	if (last_task_used_math == current)
		return;
	__asm__("fwait");
	if (last_task_used_math) {
		__asm__("fnsave %0"::"m" (last_task_used_math->tss.i387));
	}
	last_task_used_math=current;
	if (current->used_math) {
		__asm__("frstor %0"::"m" (current->tss.i387));
	} else {
		__asm__("fninit"::);
		current->used_math=1;
	}
}

/*
 *  'schedule()' is the scheduler function. This is GOOD CODE! There
 * probably won't be any reason to change this, as it should work well
 * in all circumstances (ie gives IO-bound processes good response etc).
 * The one thing you might take a look at is the signal-handler code here.
 *
 *   NOTE!!  Task 0 is the 'idle' task, which gets called when no other
 * tasks can run. It can not be killed, and it cannot sleep. The 'state'
 * information in task[0] is never used.
 */
void schedule(void)
{
	int i,next,c;

	// 等同于task_struct数组，数组内容是task_struct结构体指针。
	struct task_struct ** p;

	
/* check alarm, wake up any interruptible tasks that have got a signal */
/* 检查警报，当获取到一个信号时唤醒任何可中断休眠任务 */

	// &LAST_TASK为task数组最后一个元素的地址
	// &FIRST_TASK为task数组的地址（也就是第一个数组元素的地址）
	// 这里也就是遍历所有的task数组，真的写的骚....  大写的服了
	for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
		// 只要task数组下标p不为空就能进入
		if (*p) {

			// (*p)->alarm   也就是找到遍历的task_struct中alarm不为0的（C语言中非0即真）
			// 全局变量jiffies，初始值为0，但是这里的jiffies值为多少暂时不清楚
			if ((*p)->alarm && (*p)->alarm < jiffies) {
					// SIGALRM宏定义为14.
					// 1<<13 = 8192
					(*p)->signal |= (1<<(SIGALRM-1));
					(*p)->alarm = 0;
				}
			
			// ~为非，全部取反
			if (((*p)->signal & ~(_BLOCKABLE & (*p)->blocked)) &&
			(*p)->state==TASK_INTERRUPTIBLE)
				(*p)->state=TASK_RUNNING;
		}

/* this is the scheduler proper（适当的）: */

	// 首先调度counter值最大的，值最大也就意味着时间片最大。
	// 如果时间片都为0，那么就调度priority（优先级）值最大的
	while (1) {
		c = -1;
		next = 0;
		i = NR_TASKS;
		p = &task[NR_TASKS];
		while (--i) {
			if (!*--p)		// 为0就跳过
				continue;
			if ((*p)->state == TASK_RUNNING && (*p)->counter > c)
				c = (*p)->counter, next = i;
		}
		if (c) break;	// c不为0就退出循环。
		for(p = &LAST_TASK ; p > &FIRST_TASK ; --p)
			if (*p)
				(*p)->counter = ((*p)->counter >> 1) +
						(*p)->priority;
	}
	switch_to(next);
}

int sys_pause(void)
{
	// 设置当前运行的进程状态为休眠（可打断）状态
	current->state = TASK_INTERRUPTIBLE;

	// 走schedule方法去调度下一个执行的进程
	schedule();
	return 0;
}


void sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;

	if (current == &(init_task.task))
		panic("task[0] trying to sleep");


	tmp = *p;


	*p = current;
	current->state = TASK_UNINTERRUPTIBLE;

	// 切换任务。
	schedule();


	if (tmp)
		tmp->state=0;
}

void interruptible_sleep_on(struct task_struct **p)
{
	struct task_struct *tmp;

	if (!p)
		return;
	if (current == &(init_task.task))
		panic("task[0] trying to sleep");
	tmp=*p;
	*p=current;
repeat:	current->state = TASK_INTERRUPTIBLE;
	schedule();
	if (*p && *p != current) {
		(**p).state=0;
		goto repeat;
	}
	*p=NULL;
	if (tmp)
		tmp->state=0;
}

// 唤醒队列头的被锁标志位给休眠的进程。
void wake_up(struct task_struct **p)
{
	if (p && *p) {
		(**p).state=0;
		*p=NULL;
	}
}

/*
 * OK, here are some floppy things that shouldn't be in the kernel
 * proper. They are here because the floppy needs a timer, and this
 * was the easiest way of doing it.
 */
static struct task_struct * wait_motor[4] = {NULL,NULL,NULL,NULL};
static int  mon_timer[4]={0,0,0,0};
static int moff_timer[4]={0,0,0,0};
unsigned char current_DOR = 0x0C;

int ticks_to_floppy_on(unsigned int nr)
{
	extern unsigned char selected;
	unsigned char mask = 0x10 << nr;

	if (nr>3)
		panic("floppy_on: nr>3");
	moff_timer[nr]=10000;		/* 100 s = very big :-) */
	cli();				/* use floppy_off to turn it off */
	mask |= current_DOR;
	if (!selected) {
		mask &= 0xFC;
		mask |= nr;
	}
	if (mask != current_DOR) {
		outb(mask,FD_DOR);
		if ((mask ^ current_DOR) & 0xf0)
			mon_timer[nr] = HZ/2;
		else if (mon_timer[nr] < 2)
			mon_timer[nr] = 2;
		current_DOR = mask;
	}
	sti();
	return mon_timer[nr];
}

void floppy_on(unsigned int nr)
{
	cli();
	while (ticks_to_floppy_on(nr))
		sleep_on(nr+wait_motor);
	sti();
}

void floppy_off(unsigned int nr)
{
	moff_timer[nr]=3*HZ;
}

void do_floppy_timer(void)
{
	int i;
	unsigned char mask = 0x10;

	for (i=0 ; i<4 ; i++,mask <<= 1) {
		if (!(mask & current_DOR))
			continue;
		if (mon_timer[i]) {
			if (!--mon_timer[i])
				wake_up(i+wait_motor);
		} else if (!moff_timer[i]) {
			current_DOR &= ~mask;
			outb(current_DOR,FD_DOR);
		} else
			moff_timer[i]--;
	}
}

#define TIME_REQUESTS 64

static struct timer_list {
	long jiffies;				// 滴答数
	void (*fn)();				// 没有输入输出的函数指针
	struct timer_list * next;	// 链表
} timer_list[TIME_REQUESTS], * next_timer = NULL;

void add_timer(long jiffies, void (*fn)(void))
{
	struct timer_list * p;

	if (!fn)
		return;
	cli();
	if (jiffies <= 0)
		(fn)();
	else {
		for (p = timer_list ; p < timer_list + TIME_REQUESTS ; p++)
			if (!p->fn)
				break;
		if (p >= timer_list + TIME_REQUESTS)
			panic("No more time requests free");
		p->fn = fn;
		p->jiffies = jiffies;
		p->next = next_timer;
		next_timer = p;
		while (p->next && p->next->jiffies < p->jiffies) {
			p->jiffies -= p->next->jiffies;
			fn = p->fn;
			p->fn = p->next->fn;
			p->next->fn = fn;
			jiffies = p->jiffies;
			p->jiffies = p->next->jiffies;
			p->next->jiffies = jiffies;
			p = p->next;
		}
	}
	sti();
}

// cpl是timer_interrupt 中eax的值
void do_timer(long cpl)
{
	extern int beepcount;
	extern void sysbeepstop(void);

	if (beepcount)
		if (!--beepcount)
			sysbeepstop();

	// 用特权级来区分当前任务是在内核态还是在用户态执行
	// 并且记录
	if (cpl)
		current->utime++;
	else
		current->stime++;

	
	if (next_timer) {
		next_timer->jiffies--;
		while (next_timer && next_timer->jiffies <= 0) {
			void (*fn)(void);
			
			fn = next_timer->fn;
			next_timer->fn = NULL;
			next_timer = next_timer->next;
			(fn)();		// 调用函数指针
		}
	}

	// 0x0c = 0000 1100
	// 0xf0 = 1111 0000
	if (current_DOR & 0xf0)
		do_floppy_timer();

	// 当前任务的时间片还没到0，所以不切换任务，所以直接返回
	if ((--current->counter)>0) return;	

	// 可能为-1，既然都能执行到这一行，所以直接置为0
	current->counter=0;

	// cpl为0，也就是在内核态就直接返回。
	if (!cpl) return;		

	// 当前在用户态，并且时间片已经用完，就执行任务调度的具体逻辑
	schedule();
}

int sys_alarm(long seconds)
{
	int old = current->alarm;

	if (old)
		old = (old - jiffies) / HZ;
	current->alarm = (seconds>0)?(jiffies+HZ*seconds):0;
	return (old);
}

int sys_getpid(void)
{
	return current->pid;
}

int sys_getppid(void)
{
	return current->father;
}

int sys_getuid(void)
{
	return current->uid;
}

int sys_geteuid(void)
{
	return current->euid;
}

int sys_getgid(void)
{
	return current->gid;
}

int sys_getegid(void)
{
	return current->egid;
}

int sys_nice(long increment)
{
	if (current->priority-increment>0)
		current->priority -= increment;
	return 0;
}

void sched_init(void)
{
	int i;
	struct desc_struct * p;

	if (sizeof(struct sigaction) != 16)
		panic("Struct sigaction MUST be 16 bytes");
	set_tss_desc(gdt+FIRST_TSS_ENTRY,&(init_task.task.tss));
	set_ldt_desc(gdt+FIRST_LDT_ENTRY,&(init_task.task.ldt));
	p = gdt+2+FIRST_TSS_ENTRY;
	for(i=1;i<NR_TASKS;i++) {
		task[i] = NULL;
		p->a=p->b=0;
		p++;
		p->a=p->b=0;
		p++;
	}
/* Clear NT, so that we won't have troubles with that later on */
	__asm__("pushfl ; andl $0xffffbfff,(%esp) ; popfl");

	// 加载tr寄存器
	ltr(0);

	// 加载ldtr寄存器
	lldt(0);
	outb_p(0x36,0x43);		/* binary, mode 3, LSB/MSB, ch 0 */
	outb_p(LATCH & 0xff , 0x40);	/* LSB */
	outb(LATCH >> 8 , 0x40);	/* MSB */

	// 0x20 = 32， 将时钟中断32放入到idt表中。  属于硬中断，异步发生的。
	set_intr_gate(0x20,&timer_interrupt);
	
	outb(inb_p(0x21)&~0x01,0x21);

	// 属于软件中断，也就是异常。而异常又分为：陷进、错误、终止。对于0x80的系统调用号来说都是属于陷进
	set_system_gate(0x80,&system_call);		
}
