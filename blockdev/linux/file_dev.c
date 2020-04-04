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
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#ifdef __APPLE__
#include <fcntl.h>
#include <sys/disk.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#endif

/**@brief   Default filename.*/
static const char *fname = "ext2";

/**@brief   Image block size.*/
#define EXT4_FILEDEV_BSIZE 512

/**@brief   Image file descriptor.*/
static FILE *dev_file;

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
#ifdef __APPLE__
	/* We need to use authopen to open the device.  It asks the user for permission
	 * then give us an open fd over a socket.
	 */

	int pipe[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pipe) < 0) {
		return EFAULT;
	}

	pid_t pid = fork();
	if (pid < 0) {
		return EFAULT;
	}

	if (pid == 0) {
		close(pipe[0]);
		dup2(pipe[1], STDOUT_FILENO);
		execl("/usr/libexec/authopen", "/usr/libexec/authopen", "-stdoutpipe", "-w", "-a", fname, (char *) 0);
		exit(-1);
	}

	close(pipe[1]);

	int dev = -1;

	size_t const data_buffer_size = sizeof(struct cmsghdr) + sizeof(int);
	char data_buffer[data_buffer_size];

	struct iovec io_vec[1];
	io_vec[0].iov_base = data_buffer;
	io_vec[0].iov_len = data_buffer_size;

	socklen_t const cmsg_socket_size = CMSG_SPACE(sizeof(int));
	char cmsg_socket[cmsg_socket_size];
	struct msghdr message = { 0 };
	message.msg_iov = io_vec;
	message.msg_iovlen = 1;
	message.msg_control = cmsg_socket;
	message.msg_controllen = cmsg_socket_size;

	if (recvmsg(pipe[0], &message, 0) <= 0) {
		return EIO;
	}

	struct cmsghdr* cmsg_socket_header = CMSG_FIRSTHDR(&message);
	if (cmsg_socket_header && cmsg_socket_header->cmsg_level == SOL_SOCKET && cmsg_socket_header->cmsg_type == SCM_RIGHTS) {
		dev = *((int *) CMSG_DATA(cmsg_socket_header));
	}

	/* The fseek/ftell approach to finding the device's size does not seem
	 * to work on macOS so do it this way instead.
	 */
	uint64_t sectors = 0;
	if (ioctl(dev, DKIOCGETBLOCKCOUNT, &sectors) < 0) {
		close(dev);
		return EFAULT;
	}
	uint32_t sector_size = 0;
	if (ioctl(dev, DKIOCGETBLOCKSIZE, &sector_size) < 0) {
		close(dev);
		return EFAULT;
	}

	off_t size = sectors * sector_size;

	dev_file = fdopen(dev, "r+b");
#else
	dev_file = fopen(fname, "r+b");
#endif

	if (!dev_file)
		return EIO;

	/*No buffering at file.*/
	setbuf(dev_file, 0);

#ifndef __APPLE__
	if (fseeko(dev_file, 0, SEEK_END))
		return EFAULT;

	off_t size = ftello(dev_file);
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
	if (fseeko(dev_file, blk_id * bdev->bdif->ph_bsize, SEEK_SET))
		return EIO;
	if (!blk_cnt)
		return EOK;
	if (!fread(buf, bdev->bdif->ph_bsize * blk_cnt, 1, dev_file))
		return EIO;

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
	if (fseeko(dev_file, blk_id * bdev->bdif->ph_bsize, SEEK_SET))
		return EIO;
	if (!blk_cnt)
		return EOK;
	if (!fwrite(buf, bdev->bdif->ph_bsize * blk_cnt, 1, dev_file))
		return EIO;

	drop_cache();
	return EOK;
}
/******************************************************************************/
static int file_dev_close(struct ext4_blockdev *bdev)
{
	fclose(dev_file);
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
