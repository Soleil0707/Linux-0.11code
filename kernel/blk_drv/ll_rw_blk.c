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
// 块设备的数量，一个表示一个设备
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
	while (bh->b_lock)
		sleep_on(&bh->b_wait);
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
	cli();
	if (req->bh)
		req->bh->b_dirt = 0;
	// 如果当前没有请求，则进入if执行
	if (!(tmp = dev->current_request)) {
		// 设为当前执行请求，然后开中断调用硬盘请求项处理函数去执行
		dev->current_request = req;
		sti();
		// 直接执行当前请求
		(dev->request_fn)();
		return;
	}
	// 如果当前有在执行的请求
	// 电梯算法，让磁头的移动距离最短
	for ( ; tmp->next ; tmp=tmp->next)
		if ((IN_ORDER(tmp,req) ||
		    !IN_ORDER(tmp,tmp->next)) &&
		    IN_ORDER(req,tmp->next))
			break;
	// req被插入到了请求项队列中，等待被执行
	req->next=tmp->next;
	tmp->next=req;
	sti();
}

static void make_request(int major,int rw, struct buffer_head * bh)
{
	// 这个一个请求的结构，用它来构建请求
	struct request * req;
	int rw_ahead;

/* WRITEA/READA is special case - it is not really needed, so if the */
/* buffer is locked, we just forget about it, else it's a normal read */
	// TODO: 预读，这个函数先不看
	if (rw_ahead = (rw == READA || rw == WRITEA)) {
		if (bh->b_lock)
			return;
		if (rw == READA)
			rw = READ;
		else
			rw = WRITE;
	}
	// 不是读操作、也不是写操作
	if (rw!=READ && rw!=WRITE)
		panic("Bad block dev command, must be R/W/RA/WA");
	// 给对应缓冲块加锁
	lock_buffer(bh);
	// 如果缓冲块不干净，需要先将数据写回
	if ((rw == WRITE && !bh->b_dirt) || (rw == READ && bh->b_uptodate)) {
		unlock_buffer(bh);
		return;
	}
repeat:
/* we don't allow the write-requests to fill up the queue completely:
 * we want some room for reads: they take precedence. The last third
 * of the requests are only for reads.
 */
	// 一共32个请求项函数可以使用
	// 如果是读,就从最后开始使用;如果是写,就从整个结构的2/3处开始使用
	// TODO: 读的空间是从末尾到开头,写的空间是从2/3到开头,所以给读的空间更大,因为对用户来说,快读能带来更好的体验
	if (rw == READ)
		req = request+NR_REQUEST;
	else
		req = request+((NR_REQUEST*2)/3);
	/* find an empty request */
	// 从后往前找,找到一个空的请求项函数
	while (--req >= request)
		if (req->dev<0)
			break;
	/* if none found, sleep on new requests: check for rw_ahead */
	if (req < request) {
		if (rw_ahead) {
			unlock_buffer(bh);
			return;
		}
		sleep_on(&wait_for_request);
		goto repeat;
	}
	/* fill up the request-info, and add it to the queue */
	// 得到对应req后,需要填充其内容,以便进行请求
	req->dev = bh->b_dev;	// 设备号
	req->cmd = rw;			// 读还是写命令
	req->errors=0;
	req->sector = bh->b_blocknr<<1;	// 要读的扇区
	req->nr_sectors = 2;	// 扇区数
	req->buffer = bh->b_data;	// 对应的缓冲块位置
	req->waiting = NULL;
	req->bh = bh;
	req->next = NULL;
	// 将当前请求项加载到请求项队列中
	add_request(major+blk_dev,req);
}

// 先检查是否有相关的请求项函数，然后再使用请求项函数进行请求（制作请求并添加到请求队列）
void ll_rw_block(int rw, struct buffer_head * bh)
{
	unsigned int major;
	// 判断缓冲块对应的设备与请求项函数是否成功挂接
	// 设备号右移8位，0x300得到的为3，刚好是NR_BLK_DEV中对应的硬盘那一项，而且对应的请求项函数已经挂接
	// 确定是有效设备；确定有相关请求项函数
	if ((major=MAJOR(bh->b_dev)) >= NR_BLK_DEV ||
	!(blk_dev[major].request_fn)) {
		printk("Trying to read nonexistent block-device\n\r");
		return;
	}
	// 制作一个请求项，用于请求硬盘数据到缓冲区
	// 哪个设备、读还是写、操作的位置
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
