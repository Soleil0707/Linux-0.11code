/*
 *  linux/kernel/fork.c
 *
 *  (C) 1991  Linus Torvalds
 */

/*
 *  'fork.c' contains the help-routines for the 'fork' system call
 * (see also system_call.s), and some misc functions ('verify_area').
 * Fork is rather simple, once you get the hang of it, but the memory
 * management can be a bitch. See 'mm/mm.c': 'copy_page_tables()'
 */
#include <errno.h>

#include <linux/sched.h>
#include <linux/kernel.h>
#include <asm/segment.h>
#include <asm/system.h>

extern void write_verify(unsigned long address);

long last_pid=0;

void verify_area(void * addr,int size)
{
	unsigned long start;

	start = (unsigned long) addr;
	size += start & 0xfff;
	start &= 0xfffff000;
	start += get_base(current->ldt[2]);
	while (size>0) {
		size -= 4096;
		write_verify(start);
		start += 4096;
	}
}
// 根据进程0的代码段、数据段，设置进程1的代码段、数据段
// TODO: 这里使用的地址均为线性地址
int copy_mem(int nr,struct task_struct * p)
{
	unsigned long old_data_base,new_data_base,data_limit;
	unsigned long old_code_base,new_code_base,code_limit;

	code_limit=get_limit(0x0f);	// 01 1 11，3特权级、LDT、第2项，即当前进程（进程0）的LDT中的代码段的段选择子，依据此可以找到当前段的限长(值为640K)
	data_limit=get_limit(0x17);	// 10 1 11，3特权级、LDT、第3项
	old_code_base = get_base(current->ldt[1]);	// 段基址,其实是进程空间的基址（对于进程0而言，应该是0）
	old_data_base = get_base(current->ldt[2]);
	if (old_data_base != old_code_base)	//必须是同一个LDT的数据段和代码段
		panic("We don't support separate I&D");
	if (data_limit < code_limit)
		panic("Bad data_limit");
	new_data_base = new_code_base = nr * 0x4000000;	// 每个进程64MB的线性地址空间（4GB分给64个进程），0x0-0x3ffffff，0x4000000-0x7ffffff是进程1的空间
	p->start_code = new_code_base;
	// 设置新进程的代码段、数据段的段基址
	set_base(p->ldt[1],new_code_base);
	set_base(p->ldt[2],new_data_base);
	// 创建进程1的页表，复制进程0的页表（160个页表项。总计640KB），设置进程1的页目录项
	if (copy_page_tables(old_data_base,new_data_base,data_limit)) {
		free_page_tables(new_data_base,data_limit);
		return -ENOMEM;
	}
	return 0;
}

/*
 *  Ok, this is the main fork-routine. It copies the system process
 * information (task[nr]) and sets up the necessary registers. It
 * also copies the data segment in it's entirety.
 */
int copy_process(int nr,long ebp,long edi,long esi,long gs,long none,
		long ebx,long ecx,long edx,
		long fs,long es,long ds,
		long eip,long cs,long eflags,long esp,long ss)
{
	struct task_struct *p;
	int i;
	struct file *f;

	p = (struct task_struct *) get_free_page(); //申请一个页，用于进程1的task_struct和内核栈，页面已经清零、返回的是物理地址
	// TODO: 这里验证了每个进程都一个内核栈
	if (!p)
		return -EAGAIN;
	task[nr] = p;	//进程1与task数组挂接，此时nr的值应该是1，意味着可以被调度了
	*p = *current;	/* NOTE! this doesn't copy the supervisor stack */	//这一行将进程0的task_struct完全拷贝给进程1，但内核栈没有拷贝，因为内核栈处于页的末端
	p->state = TASK_UNINTERRUPTIBLE;
	p->pid = last_pid;
	p->father = current->pid;
	p->counter = p->priority;
	p->signal = 0;
	p->alarm = 0;
	p->leader = 0;		/* process leadership doesn't inherit */
	p->utime = p->stime = 0;
	p->cutime = p->cstime = 0;
	p->start_time = jiffies;
	p->tss.back_link = 0;
	p->tss.esp0 = PAGE_SIZE + (long) p;		// 内核栈指针的位置，即该页面的尾端
	p->tss.ss0 = 0x10;	// 10000
	p->tss.eip = eip;						// 这行与下一行控制进程1开始运行时执行的代码
	p->tss.eflags = eflags;
	p->tss.eax = 0;							// 写死为0，作为返回地址
	p->tss.ecx = ecx;
	p->tss.edx = edx;
	p->tss.ebx = ebx;
	p->tss.esp = esp;
	p->tss.ebp = ebp;
	p->tss.esi = esi;
	p->tss.edi = edi;
	p->tss.es = es & 0xffff;
	p->tss.cs = cs & 0xffff;
	p->tss.ss = ss & 0xffff;
	p->tss.ds = ds & 0xffff;
	p->tss.fs = fs & 0xffff;
	p->tss.gs = gs & 0xffff;
	p->tss.ldt = _LDT(nr);	// 进程1的LDT与LDT表进行挂接
	p->tss.trace_bitmap = 0x80000000;
	if (last_task_used_math == current)
		__asm__("clts ; fnsave %0"::"m" (p->tss.i387));
	if (copy_mem(nr,p)) {
		task[nr] = NULL;
		free_page((long) p);
		return -EAGAIN;
	}
	// 进程1共享进程0的文件
	for (i=0; i<NR_OPEN;i++)
		if (f=p->filp[i])
			f->f_count++;
	if (current->pwd)	// 当前目录的i节点(对于进程0而言，这些都是空的，所有进程1的也是空的)
		current->pwd->i_count++;
	if (current->root)	// 根目录的i节点
		current->root->i_count++;
	if (current->executable)	// 执行文件的i节点
		current->executable->i_count++;
	// 进程1的ldt、tss挂载到GDT上（GDT布局：NULL、内核CS、内核DS、NULL、TSS0、LDT0、TSS1、LDT1...）
	set_tss_desc(gdt+(nr<<1)+FIRST_TSS_ENTRY,&(p->tss));
	set_ldt_desc(gdt+(nr<<1)+FIRST_LDT_ENTRY,&(p->ldt));
	// 设置为就绪态、可以调度了
	p->state = TASK_RUNNING;	/* do this last, just in case */
	return last_pid;
}

int find_empty_process(void)
{
	int i;

	repeat:
		if ((++last_pid)<0) last_pid=1;
		for(i=0 ; i<NR_TASKS ; i++)
			if (task[i] && task[i]->pid == last_pid) goto repeat;
	for(i=1 ; i<NR_TASKS ; i++)
		if (!task[i])
			return i;
	return -EAGAIN;
}
