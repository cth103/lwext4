/*
 * Copyright (c) 2013 Grzegorz Kostka (kostka.grzegorz@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <ext4_config.h>
#include <ext4_blockdev.h>
#include <ext4_errno.h>
#include <ext4_debug.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <fcntl.h>
#include <sys/disk.h>
#include <stdlib.h>
#include <sys/socket.h>
#endif

/**@brief   Default filename.*/
static const char *fname = "ext2";

/**@brief   Image block size.*/
#define EXT4_FILEDEV_BSIZE 512

/**@brief   Image file descriptor.*/
static int dev_file;

#define DROP_LINUXCACHE_BUFFERS 0

/**********************BLOCKDEV INTERFACE**************************************/
static int file_dev_open(struct ext4_blockdev *bdev);
static int file_dev_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id,
			 uint32_t blk_cnt);
static int file_dev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			  uint64_t blk_id, uint32_t blk_cnt);
static int file_dev_close(struct ext4_blockdev *bdev);

/******************************************************************************/
EXT4_BLOCKDEV_STATIC_INSTANCE(file_dev, EXT4_FILEDEV_BSIZE, 0, file_dev_open,
		file_dev_bread, file_dev_bwrite, file_dev_close, 0, 0);

/******************************************************************************/
static int file_dev_open(struct ext4_blockdev *bdev)
{
	dev_file = open(fname, O_RDWR);

	if (dev_file < 0) {
		printf("open of %s failed %d\n", fname, errno);
		return EIO;
	}

#ifdef __APPLE__
	/* The lseek approach to finding the device's size does not seem
	 * to work on macOS so do it this way instead.
	 */
	uint64_t sectors = 0;
	if (ioctl(dev_file, DKIOCGETBLOCKCOUNT, &sectors) < 0) {
		printf("ioctl DKIOCGETBLOCKCOUNT failed %d\n", errno);
		close(dev_file);
		return EFAULT;
	}
	uint32_t sector_size = 0;
	if (ioctl(dev_file, DKIOCGETBLOCKSIZE, &sector_size) < 0) {
		printf("ioctl DKIOCGETBLOCKSIZE failed %d\n", errno);
		close(dev_file);
		return EFAULT;
	}

	off_t size = sectors * sector_size;
#else
	off_t size = lseek(dev_file, 0, SEEK_END);
	if (size == ((off_t) -1)) {
		return EFAULT;
	}

#endif

	file_dev.part_offset = 0;
	file_dev.part_size = size;
	file_dev.bdif->ph_bcnt = file_dev.part_size / file_dev.bdif->ph_bsize;

	return EOK;
}

/******************************************************************************/

static int file_dev_bread(struct ext4_blockdev *bdev, void *buf, uint64_t blk_id,
			 uint32_t blk_cnt)
{
	if (lseek(dev_file, blk_id * bdev->bdif->ph_bsize, SEEK_SET) < 0) {
		printf("lseek failed %d\n", errno);
		return EIO;
	}
	if (!blk_cnt)
		return EOK;
	if (read(dev_file, buf, bdev->bdif->ph_bsize * blk_cnt) < 0) {
		printf("read failed %d\n", errno);
		return EIO;
	}

	return EOK;
}

static void drop_cache(void)
{
#if defined(__linux__) && DROP_LINUXCACHE_BUFFERS
	int fd;
	char *data = "3";

	sync();
	fd = open("/proc/sys/vm/drop_caches", O_WRONLY);
	write(fd, data, sizeof(char));
	close(fd);
#endif
}

/******************************************************************************/
static int file_dev_bwrite(struct ext4_blockdev *bdev, const void *buf,
			  uint64_t blk_id, uint32_t blk_cnt)
{
	if (lseek(dev_file, blk_id * bdev->bdif->ph_bsize, SEEK_SET) < 0) {
		printf("fseeko failed %d\n", errno);
		return EIO;
	}
	if (!blk_cnt)
		return EOK;
	if (write(dev_file, buf, bdev->bdif->ph_bsize * blk_cnt) < 0) {
		printf("write failed %d\n", errno);
		return EIO;
	}

	drop_cache();
	return EOK;
}
/******************************************************************************/
static int file_dev_close(struct ext4_blockdev *bdev)
{
	close(dev_file);
	return EOK;
}

/******************************************************************************/
struct ext4_blockdev *file_dev_get(void)
{
	return &file_dev;
}
/******************************************************************************/
void file_dev_name_set(const char *n)
{
	fname = n;
}
/******************************************************************************/
