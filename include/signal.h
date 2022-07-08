#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <sys/types.h>

typedef int sig_atomic_t;
typedef unsigned int sigset_t;		/* 32 bits */

#define _NSIG             32
#define NSIG		_NSIG

// 挂断控制终端或进程
#define SIGHUP		 1

// 来自键盘的中断
#define SIGINT		 2

// 来自键盘的退出
#define SIGQUIT		 3

// 非法指令
#define SIGILL		 4

// 跟踪断点
#define SIGTRAP		 5

// 异常结束
#define SIGABRT		 6

// 同上（猜测:异常结束）
#define SIGIOT		 6

// 没有使用
#define SIGUNUSED	 7

// 协处理器出错
#define SIGFPE		 8

// 强迫进程终止
#define SIGKILL		 9

// 用户信号1，进程可使用
#define SIGUSR1		10

// 无效内存引用
#define SIGSEGV		11

// 用户信号2，进程可使用
#define SIGUSR2		12

// 管道写出错，无读者
#define SIGPIPE		13

// 实时定时器报警
#define SIGALRM		14

// 进程终止
#define SIGTERM		15

// 栈出错
#define SIGSTKFLT	16

// 子进程停止或者被终止
#define SIGCHLD		17

// 恢复进程继续执行
#define SIGCONT		18

// 停止进程执行
#define SIGSTOP		19

// tyy发出停止进程，可忽略
#define SIGTSTP		20

// 后台进程请求输入
#define SIGTTIN		21

// 后台进程请求输出
#define SIGTTOU		22

/* Ok, I haven't implemented sigactions, but trying to keep headers POSIX */
#define SA_NOCLDSTOP	1
#define SA_NOMASK	0x40000000
#define SA_ONESHOT	0x80000000

#define SIG_BLOCK          0	/* for blocking signals */
#define SIG_UNBLOCK        1	/* for unblocking signals */
#define SIG_SETMASK        2	/* for setting the signal mask */

#define SIG_DFL		((void (*)(int))0)	/* default signal handling */
#define SIG_IGN		((void (*)(int))1)	/* ignore signal */

struct sigaction {
	void (*sa_handler)(int);
	sigset_t sa_mask;
	int sa_flags;
	void (*sa_restorer)(void);
};

void (*signal(int _sig, void (*_func)(int)))(int);
int raise(int sig);
int kill(pid_t pid, int sig);
int sigaddset(sigset_t *mask, int signo);
int sigdelset(sigset_t *mask, int signo);
int sigemptyset(sigset_t *mask);
int sigfillset(sigset_t *mask);
int sigismember(sigset_t *mask, int signo); /* 1 - is, 0 - not, -1 error */
int sigpending(sigset_t *set);
int sigprocmask(int how, sigset_t *set, sigset_t *oldset);
int sigsuspend(sigset_t *sigmask);
int sigaction(int sig, struct sigaction *act, struct sigaction *oldact);

#endif /* _SIGNAL_H */
