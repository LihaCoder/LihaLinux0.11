/*
 *  linux/mm/page.s
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 * page.s contains the low-level page-exception code.
 * the real work is done in mm.c
 */

.globl _page_fault

/*
	do_no_page:缺页处理
	do_wp_page:保护相关的处理
*/
/*
	所以这个(esp)是PDE或者PTE
*/
_page_fault:
	// 错误码是CPU自动压入栈的
	xchgl %eax,(%esp)	// 互相交换    		(%esp)是将当前栈中esp的地址取值出来放入到eax中
	
	pushl %ecx			// 压栈操作
	pushl %edx			// 压栈操作
	push %ds			// 压栈操作
	push %es			// 压栈操作
	push %fs			// 压栈操作
	
	movl $0x10,%edx		// 将0001 0 00 0放入edx中
	mov %dx,%ds			// 低16位放到ds中,这里dddd（选择子），所以这里是定位到内核的数据段
	mov %dx,%es			// 低16位放到es中,这里dddd（选择子），所以这里是定位到内核的数据段
	mov %dx,%fs			// 低16位放到fs中,这里dddd（选择子），所以这里是定位到内核的数据段
	movl %cr2,%edx		// cr2存放的是页面故障的线性地址
	
	pushl %edx			// 页面故障的线性地址压入栈，目的是为了栈传参
	pushl %eax			// 错误码eax压栈，目的是为了栈传参
	
	testl $1,%eax		// test指令是与操作，但是会把结果相关的内容放入到eflags寄存器中
						// 1 与任何值，只要被与的值最后一位不为1就为0.
						
	jne 1f				// 不为0就跳转到向后1代码段
	
	call _do_no_page	// 处理为0，为0就是缺页
	
	jmp 2f
1:	call _do_wp_page	// 不为0执行的，为1就是因为保护引发的fault

2:	addl $8,%esp		// 为0继续执行的，将前面压栈的内容给抹除
	pop %fs
	pop %es
	pop %ds
	popl %edx
	popl %ecx
	popl %eax
	iret
