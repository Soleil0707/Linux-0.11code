/*
 *  linux/kernel/blk_drv/ramdisk.c
 *
 *  Written by Theodore Ts'o, 12/2/91
 */

#include <string.h>

#include <linux/config.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <asm/system.h>
#include <asm/segment.h>
#include <asm/memory.h>

#define MAJOR_NR 1
#include "blk.h"

char	*rd_start;
int	rd_length = 0;

void do_rd_request(void)
{
	int	len;
	char	*addr;

	INIT_REQUEST;
	addr = rd_start + (CURRENT->sector << 9);
	len = CURRENT->nr_sectors << 9;
	if ((MINOR(CURRENT->dev) != 1) || (addr+len > rd_start+rd_length)) {
		end_request(0);
		goto repeat;
	}
	if (CURRENT-> cmd == WRITE) {
		(void ) memcpy(addr,
			      CURRENT->buffer,
			      len);
	} else if (CURRENT->cmd == READ) {
		(void) memcpy(CURRENT->buffer, 
			      addr,
			      len);
	} else
		panic("unknown ramdisk-command");
	end_request(1);
	goto repeat;
}

/*
 * Returns amount of memory which needs to be reserved.
 * @return 返回虚拟盘占用的内存空间,即传入的参数length
 * @parms  主内存的起始位置,虚拟盘的大小	
 */
long rd_init(long mem_start, int length)
{
	int	i;
	char	*cp;
	// do_rd_reques函数(DEVICE_REQUEST,虚拟盘的请求项处理函数)与blk_dev[7](请求项函数处理结构)对接
	blk_dev[MAJOR_NR].request_fn = DEVICE_REQUEST;	// 之后内核可以调用do_rd_reques函数处理与虚拟盘相关的请求项操作
	rd_start = (char *) mem_start;
	rd_length = length;
	cp = rd_start;
	for (i=0; i < length; i++)	// 清空了虚拟盘所占区域
		*cp++ = '\0';
	return(length);
}

/*
 * If the root device is the ram disk, try to load it.
 * In order to do this, the root device is originally set to the
 * floppy, and we later change it to be ram disk.
 */
void rd_load(void)
{
	struct buffer_head *bh;
	struct super_block	s;
	// 从256扇区开始格式化（第一扇区是bootsect、后四个扇区是setup、紧接着是包含head的system模块，占240个扇区）
	int		block = 256;	/* Start at block 256 */
	int		i = 1;
	int		nblocks;
	char		*cp;		/* Move pointer */
	
	if (!rd_length)
		return;
	printk("Ram disk: %d bytes, starting at 0x%x\n", rd_length,
		(int) rd_start);
	if (MAJOR(ROOT_DEV) != 2)
		return;
	// 从软盘预读数据块，即引导块和超级块等（此时的根设备为软盘）
	// 读取了三个块，分别是257、256、258
	bh = breada(ROOT_DEV,block+1,block,block+2,-1);
	if (!bh) {
		printk("Disk error while looking for ramdisk!\n");
		return;
	}
	*((struct d_super_block *) &s) = *((struct d_super_block *) bh->b_data);
	brelse(bh);
	// 检查是否为minix文件系统
	if (s.s_magic != SUPER_MAGIC)
		/* No ram disk image present, assume normal floppy boot */
		return;
	// 算出虚拟盘的块数
	nblocks = s.s_nzones << s.s_log_zone_size;
	if (nblocks > (rd_length >> BLOCK_SIZE_BITS)) {
		printk("Ram disk image too big!  (%d blocks, %d avail)\n", 
			nblocks, rd_length >> BLOCK_SIZE_BITS);
		return;
	}
	printk("Loading %d bytes into ram disk... 0000k", 
		nblocks << BLOCK_SIZE_BITS);
	cp = rd_start;
	// 将软盘上准备格式化用的根文件系统复制到虚拟盘
	while (nblocks) {
		if (nblocks > 2) 
			bh = breada(ROOT_DEV, block, block+1, block+2, -1);
		else
			bh = bread(ROOT_DEV, block);
		if (!bh) {
			printk("I/O error on block %d, aborting load\n", 
				block);
			return;
		}
		(void) memcpy(cp, bh->b_data, BLOCK_SIZE);
		// 释放缓冲块
		brelse(bh);
		printk("\010\010\010\010\010%4dk",i);
		cp += BLOCK_SIZE;
		block++;
		nblocks--;
		i++;
	}
	printk("\010\010\010\010\010done \n");
	// 将虚拟盘设置为根设备
	ROOT_DEV=0x0101;
}
