/*
 *  linux/init/main.c
 *
 *  (C) 1991  Linus Torvalds
 */

#define __LIBRARY__
#include <unistd.h>
#include <time.h>

/*
 * we need this inline - forking from kernel space will result
 * in NO COPY ON WRITE (!!!), until an execve is executed. This
 * is no problem, but for the stack. This is handled by not letting
 * main() use the stack at all after fork(). Thus, no function
 * calls - which means inline code for fork too, as otherwise we
 * would use the stack upon exit from 'fork()'.
 *
 * Actually only pause and fork are needed inline, so that there
 * won't be any messing with the stack from main(), but we define
 * some others too.
 */
static inline _syscall0(int,fork)
static inline _syscall0(int,pause)
static inline _syscall1(int,setup,void *,BIOS)
static inline _syscall0(int,sync)

#include <linux/tty.h>
#include <linux/sched.h>
#include <linux/head.h>
#include <asm/system.h>
#include <asm/io.h>

#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

#include <linux/fs.h>

static char printbuf[1024];

extern int vsprintf();
extern void init(void);
extern void blk_dev_init(void);
extern void chr_dev_init(void);
extern void hd_init(void);
extern void floppy_init(void);
extern void mem_init(long start, long end);
extern long rd_init(long mem_start, int length);
extern long kernel_mktime(struct tm * tm);
extern long startup_time;

/*
 * This is set up by the setup-routine at boot-time
 */
#define EXT_MEM_K (*(unsigned short *)0x90002)
#define DRIVE_INFO (*(struct drive_info *)0x90080)	// 此处存放硬盘参数表	
#define ORIG_ROOT_DEV (*(unsigned short *)0x901FC)	// 该地址存储了根设备号

/*
 * Yeah, yeah, it's ugly, but I cannot find how to do this correctly
 * and this seems to work. I anybody has more info on the real-time
 * clock I'd be interested. Most of this was trial and error, and some
 * bios-listing reading. Urghh.
 */

#define CMOS_READ(addr) ({ \
outb_p(0x80|addr,0x70); \
inb_p(0x71); \
})

#define BCD_TO_BIN(val) ((val)=((val)&15) + ((val)>>4)*10)

static void time_init(void)
{
	struct tm time;

	do {
		time.tm_sec = CMOS_READ(0);
		time.tm_min = CMOS_READ(2);
		time.tm_hour = CMOS_READ(4);
		time.tm_mday = CMOS_READ(7);
		time.tm_mon = CMOS_READ(8);
		time.tm_year = CMOS_READ(9);
	} while (time.tm_sec != CMOS_READ(0));
	BCD_TO_BIN(time.tm_sec);
	BCD_TO_BIN(time.tm_min);
	BCD_TO_BIN(time.tm_hour);
	BCD_TO_BIN(time.tm_mday);
	BCD_TO_BIN(time.tm_mon);
	BCD_TO_BIN(time.tm_year);
	time.tm_mon--;
	startup_time = kernel_mktime(&time);
}

static long memory_end = 0;
static long buffer_memory_end = 0;
static long main_memory_start = 0;

struct drive_info { char dummy[32]; } drive_info;

void main(void)		/* This really IS void, no error here. */
{			/* The startup routine assumes (well, ...) this */
/*
 * Interrupts are still disabled. Do necessary setups, then
 * enable them
 */
// 此时仍处于关中断的状态
 	ROOT_DEV = ORIG_ROOT_DEV;   // 根设备为软盘
 	drive_info = DRIVE_INFO;    // 硬盘信息
// 接下来开始规划物理内存,包括了缓冲区,虚拟盘,主内存(还有一部分存放了内核的代码和数据,在地址0往后的一部分空间,这部分不做其他规划)
// 整个结构为:内核代码,内核数据,缓冲区,虚拟盘,主内存
	memory_end = (1<<20) + (EXT_MEM_K<<10); // 内存总量,单位是字节，操作系统自带了1MB
	memory_end &= 0xfffff000;               // 页对齐,忽略不足一页的内存量
	if (memory_end > 16*1024*1024)          // 如果内存大于16MB(物理内存),则以16MB计
		memory_end = 16*1024*1024;
	if (memory_end > 12*1024*1024)          // 如果内存大于12MB,设置缓冲区为4MB
		buffer_memory_end = 4*1024*1024;
	else if (memory_end > 6*1024*1024)      // 如果内存介于6MB-12MB,设置缓冲区为2MB
		buffer_memory_end = 2*1024*1024;
	else                                    // 如果内存小于1MB,设置缓冲区为1MB
		buffer_memory_end = 1*1024*1024;
	main_memory_start = buffer_memory_end;  // 缓冲区之后即为主内存
#ifdef RAMDISK                              // 如果定义了虚拟盘,RAMDISK在makefile中被定义
	main_memory_start += rd_init(main_memory_start, RAMDISK*1024);  // 挂接虚拟盘请求项函数,清空虚拟盘区域
#endif
	mem_init(main_memory_start,memory_end); //初始化主内存,规划了主内存区的页面使用次数
	trap_init();    // 将中断处理服务程序与IDT挂接,重建中断服务体系
	blk_dev_init(); // 初始化块设备请求项，一共32个，但还未与具体设备挂接
	chr_dev_init();	// 空函数，本来是用来初始化字符设备的
	tty_init();		// 真正用来初始化字符设备的，teletype初始化
	time_init();	// 开机启动时间
	sched_init();	// 初始化进程0
	buffer_init(buffer_memory_end);	// 初始化缓冲区
	hd_init();	// 初始化硬盘
	floppy_init();	// 初始化软盘，类似于硬盘
	sti();			// 开启中断，EFLAGS寄存器中的IF置为1，表示允许中断
	move_to_user_mode();	// 特权级翻转，由0至3
	if (!fork()) {		/* we count on this going ok */ //这一行创建了进程1
		init();
	}
/*
 *   NOTE!!   For any other task 'pause()' would mean we have to get a
 * signal to awaken, but task0 is the sole exception (see 'schedule()')
 * as task 0 gets activated at every idle moment (when no other tasks
 * can run). For task0 'pause()' just means we go check if some other
 * task can run, and if not we return here.
 */
	// 此时进程1创建完成、正在运行进程0
	for(;;) pause();	// pause函数为系统调用，进程0主动让出CPU
}

static int printf(const char *fmt, ...)
{
	va_list args;
	int i;

	va_start(args, fmt);
	write(1,printbuf,i=vsprintf(printbuf, fmt, args));
	va_end(args);
	return i;
}

static char * argv_rc[] = { "/bin/sh", NULL };
static char * envp_rc[] = { "HOME=/", NULL };

static char * argv[] = { "-/bin/sh",NULL };
static char * envp[] = { "HOME=/usr/root", NULL };

void init(void)
{
	int pid,i;

	setup((void *) &drive_info);
	(void) open("/dev/tty0",O_RDWR,0);
	(void) dup(0);
	(void) dup(0);
	printf("%d buffers = %d bytes buffer space\n\r",NR_BUFFERS,
		NR_BUFFERS*BLOCK_SIZE);
	printf("Free mem: %d bytes\n\r",memory_end-main_memory_start);
	if (!(pid=fork())) {
		close(0);
		if (open("/etc/rc",O_RDONLY,0))
			_exit(1);
		execve("/bin/sh",argv_rc,envp_rc);
		_exit(2);
	}
	if (pid>0)
		while (pid != wait(&i))
			/* nothing */;
	while (1) {
		if ((pid=fork())<0) {
			printf("Fork failed in init\r\n");
			continue;
		}
		if (!pid) {
			close(0);close(1);close(2);
			setsid();
			(void) open("/dev/tty0",O_RDWR,0);
			(void) dup(0);
			(void) dup(0);
			_exit(execve("/bin/sh",argv,envp));
		}
		while (1)
			if (pid == wait(&i))
				break;
		printf("\n\rchild %d died with code %04x\n\r",pid,i);
		sync();
	}
	_exit(0);	/* NOTE! _exit, not exit() */
}
