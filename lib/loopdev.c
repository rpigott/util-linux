/*
 * Copyright (C) 2011 Karel Zak <kzak@redhat.com>
 *
 * -- based on mount/losetup.c
 *
 * Simple library for work with loop devices.
 *
 *  - requires kernel 2.6.x
 *  - reads info from /sys/block/loop<N>/loop/<attr> (new kernels)
 *  - reads info by ioctl
 *  - supports *unlimited* number of loop devices
 *  - supports /dev/loop<N> as well as /dev/loop/<N>
 *  - minimize overhead (fd, loopinfo, ... are shared for all operations)
 *  - setup (associate device and backing file)
 *  - delete (dis-associate file)
 *  - old LOOP_{SET,GET}_STATUS (32bit) ioctls are unsupported
 *  - extendible
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <inttypes.h>
#include <dirent.h>
#include <linux/posix_types.h>

#include "linux_version.h"
#include "c.h"
#include "sysfs.h"
#include "pathnames.h"
#include "loopdev.h"
#include "canonicalize.h"

#define loopcxt_ioctl_enabled(_lc)	(!((_lc)->flags & LOOPDEV_FL_NOIOCTL))

/*
 * @lc: context
 * @device: device name, absolute device path or NULL to reset the current setting
 *
 * Sets device, absolute paths (e.g. "/dev/loop<N>") are unchanged, device
 * names ("loop<N>") are converted to the path (/dev/loop<N> or to
 * /dev/loop/<N>)
 *
 * Returns: <0 on error, 0 on success
 */
int loopcxt_set_device(struct loopdev_cxt *lc, const char *device)
{
	if (!lc)
		return -EINVAL;

	if (lc->fd >= 0)
		close(lc->fd);
	lc->fd = -1;
	lc->has_info = 0;
	*lc->device = '\0';

	/* set new */
	if (device) {
		if (*device != '/') {
			const char *dir = _PATH_DEV;

			/* compose device name for /dev/loop<n> or /dev/loop/<n> */
			if (lc->flags & LOOPDEV_FL_DEVSUBDIR) {
				if (strlen(device) < 5)
					return -1;
				device += 4;
				dir = _PATH_DEV_LOOP "/";	/* _PATH_DEV uses tailing slash */
			}
			snprintf(lc->device, sizeof(lc->device), "%s%s",
				dir, device);
		} else {
			strncpy(lc->device, device, sizeof(lc->device));
			lc->device[sizeof(lc->device) - 1] = '\0';
		}
	}

	sysfs_deinit(&lc->sysfs);
	return 0;
}

/*
 * @lc: context
 * @flags: LOOPDEV_FL_* flags
 *
 * Initilize loop handler.
 *
 * Returns: <0 on error, 0 on success.
 */
int loopcxt_init(struct loopdev_cxt *lc, int flags)
{
	if (!lc)
		return -EINVAL;

	memset(lc, 0, sizeof(*lc));
	lc->flags = flags;
	loopcxt_set_device(lc, NULL);

	if (!(lc->flags && LOOPDEV_FL_NOSYSFS) &&
	    get_linux_version() >= KERNEL_VERSION(2,6,37))
		/*
		 * Use only sysfs for basic information about loop devices
		 */
		lc->flags |= LOOPDEV_FL_NOIOCTL;

	return 0;
}

/*
 * @lc: context
 *
 * Deinitialize loop context
 */
void loopcxt_deinit(struct loopdev_cxt *lc)
{
	if (!lc)
		return;

	free(lc->filename);
	lc->filename = NULL;

	loopcxt_set_device(lc, NULL);
	loopcxt_deinit_iterator(lc);
}

/*
 * @lc: context
 *
 * Returns newly allocated device path.
 */
char *loopcxt_strdup_device(struct loopdev_cxt *lc)
{
	if (!lc || !*lc->device)
		return NULL;
	return strdup(lc->device);
}

/*
 * @lc: context
 *
 * Returns pointer device name in the @lc struct.
 */
const char *loopcxt_get_device(struct loopdev_cxt *lc)
{
	return lc ? lc->device : NULL;
}

/*
 * @lc: context
 *
 * Returns pointer to the sysfs context (see lib/sysfs.c)
 */
struct sysfs_cxt *loopcxt_get_sysfs(struct loopdev_cxt *lc)
{
	if (!lc || !*lc->device || (lc->flags & LOOPDEV_FL_NOSYSFS))
		return NULL;

	if (!lc->sysfs.devno) {
		dev_t devno = sysfs_devname_to_devno(lc->device, NULL);
		if (!devno)
			return NULL;

		if (sysfs_init(&lc->sysfs, devno, NULL))
			return NULL;
	}
	return &lc->sysfs;
}

/*
 * @lc: context
 *
 * Returns: file descriptor to the open loop device or <0 on error. The mode
 *          depends on LOOPDEV_FL_{RDWR,RDONLY} context flags. Default is
 *          read-only.
 */
int loopcxt_get_fd(struct loopdev_cxt *lc)
{
	if (!lc || !*lc->device)
		return -1;

	if (lc->fd < 0)
		lc->fd = open(lc->device, lc->flags & LOOPDEV_FL_RDWR ?
				O_RDWR : O_RDONLY);
	return lc->fd;
}

/*
 * @lc: context
 * @flags: LOOPITER_FL_* flags
 *
 * Iterator allows to scan list of the free or used loop devices.
 *
 * Returns: <0 on error, 0 on success
 */
int loopcxt_init_iterator(struct loopdev_cxt *lc, int flags)
{
	struct loopdev_iter *iter;
	struct stat st;

	if (!lc)
		return -EINVAL;

	iter = &lc->iter;

	/* always zeroize
	 */
	memset(iter, 0, sizeof(*iter));
	iter->ncur = -1;
	iter->flags = flags;
	iter->default_check = 1;

	if (!lc->extra_check) {
		/*
		 * Check for /dev/loop/<N> subdirectory
		 */
		if (!(lc->flags && LOOPDEV_FL_DEVSUBDIR) &&
		    stat(_PATH_DEV_LOOP, &st) == 0 && S_ISDIR(st.st_mode))
			lc->flags |= LOOPDEV_FL_DEVSUBDIR;

		lc->extra_check = 1;
	}
	return 0;
}

/*
 * @lc: context
 *
 * Returns: <0 on error, 0 on success
 */
int loopcxt_deinit_iterator(struct loopdev_cxt *lc)
{
	struct loopdev_iter *iter = &lc->iter;

	if (!lc)
		return -EINVAL;

	iter = &lc->iter;

	free(iter->minors);
	if (iter->proc)
		fclose(iter->proc);
	iter->minors = NULL;
	iter->proc = NULL;
	iter->done = 1;
	return 0;
}

/*
 * Same as loopcxt_set_device, but also checks if the device is
 * associeted with any file.
 *
 * Returns: <0 on error, 0 on success, 1 device does not match with
 *         LOOPITER_FL_{USED,FREE} flags.
 */
static int loopiter_set_device(struct loopdev_cxt *lc, const char *device)
{
	int rc = loopcxt_set_device(lc, device);
	int used;

	if (rc)
		return rc;

	if (!(lc->iter.flags & LOOPITER_FL_USED) &&
	    !(lc->iter.flags & LOOPITER_FL_FREE))
		return 0;	/* caller does not care about device status */

	used = loopcxt_get_offset(lc, NULL) == 0;

	if ((lc->iter.flags & LOOPITER_FL_USED) && used)
		return 0;

	if ((lc->iter.flags & LOOPITER_FL_FREE) && !used)
		return 0;

	loopcxt_set_device(lc, NULL);
	return 1;
}

static int cmpnum(const void *p1, const void *p2)
{
	return (* (int *) p1) > (* (int *) p2);
}

/*
 * The classic scandir() is more expensive and less portable.
 * We needn't full loop device names -- loop numbers (loop<N>)
 * are enough.
 */
static int loop_scandir(const char *dirname, int **ary, int hasprefix)
{
	DIR *dir;
	struct dirent *d;
	unsigned int n, count = 0, arylen = 0;

	if (!dirname || !ary)
		return 0;
	dir = opendir(dirname);
	if (!dir)
		return 0;

	free(*ary);
	*ary = NULL;

	while((d = readdir(dir))) {
#ifdef _DIRENT_HAVE_D_TYPE
		if (d->d_type != DT_BLK && d->d_type != DT_UNKNOWN &&
		    d->d_type != DT_LNK)
			continue;
#endif
		if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
			continue;

		if (hasprefix) {
			/* /dev/loop<N> */
			if (sscanf(d->d_name, "loop%u", &n) != 1)
				continue;
		} else {
			/* /dev/loop/<N> */
			char *end = NULL;

			n = strtol(d->d_name, &end, 10);
			if (d->d_name == end || (end && *end) || errno)
				continue;
		}
		if (n < LOOPDEV_DEFAULT_NNODES)
			continue;			/* ignore loop<0..7> */

		if (count + 1 > arylen) {
			int *tmp;

			arylen += 1;

			tmp = realloc(*ary, arylen * sizeof(int));
			if (!tmp) {
				free(*ary);
				return -1;
			}
			*ary = tmp;
		}
		(*ary)[count++] = n;
	}
	if (count)
		qsort(*ary, count, sizeof(int), cmpnum);

	closedir(dir);
	return count;
}

/*
 * @lc: context, has to initialized by loopcxt_init_iterator()
 *
 * Returns: 0 on success, -1 on error, 1 at the end of scanning. The details
 *          about the current loop device are available by
 *          loopcxt_get_{fd,backing_file,device,offset, ...} functions.
 */
int loopcxt_next(struct loopdev_cxt *lc)
{
	struct loopdev_iter *iter;

	if (!lc)
		return -EINVAL;
	iter = &lc->iter;
	if (iter->done)
		return 1;

	/* A) Look for used loop devices in /proc/partitions ("losetup -a" only)
	 */
	if (iter->flags & LOOPITER_FL_USED) {
		char buf[BUFSIZ];

		if (!iter->proc)
			iter->proc = fopen(_PATH_PROC_PARTITIONS, "r");

		while (iter->proc && fgets(buf, sizeof(buf), iter->proc)) {
			unsigned int m;
			char name[128];

			if (sscanf(buf, " %u %*s %*s %128[^\n ]",
				   &m, name) != 2 || m != LOOPDEV_MAJOR)
				continue;

			if (loopiter_set_device(lc, name) == 0)
				return 0;
		}

		goto done;
	}

	/* B) Classic way, try first eight loop devices (default number
	 *    of loop devices). This is enough for 99% of all cases.
	 */
	if (iter->default_check) {
		for (++iter->ncur; iter->ncur < LOOPDEV_DEFAULT_NNODES;
							iter->ncur++) {
			char name[16];
			snprintf(name, sizeof(name), "loop%d", iter->ncur);

			if (loopiter_set_device(lc, name) == 0)
				return 0;
		}
		iter->default_check = 0;
	}

	/* C) the worst possibility, scan whole /dev or /dev/loop/<N>
	 */
	if (!iter->minors) {
		iter->nminors = (lc->flags & LOOPDEV_FL_DEVSUBDIR) ?
			loop_scandir(_PATH_DEV_LOOP, &iter->minors, 0) :
			loop_scandir(_PATH_DEV, &iter->minors, 1);
		iter->ncur = -1;
	}
	for (++iter->ncur; iter->ncur < iter->nminors; iter->ncur++) {
		char name[16];
		snprintf(name, sizeof(name), "loop%d", iter->minors[iter->ncur]);

		if (loopiter_set_device(lc, name) == 0)
			return 0;
	}
done:
	loopcxt_deinit_iterator(lc);
	return 1;
}

/*
 * @device: path to device
 */
int is_loopdev(const char *device)
{
	struct stat st;

	if (!device)
		return 0;

	return (stat(device, &st) == 0 &&
		S_ISBLK(st.st_mode) &&
		major(st.st_rdev) == LOOPDEV_MAJOR);
}

/*
 * @lc: context
 *
 * Returns result from LOOP_GET_STAT64 ioctl or NULL on error.
 */
struct loop_info64 *loopcxt_get_info(struct loopdev_cxt *lc)
{
	int fd;

	if (!lc)
		return NULL;
	if (lc->has_info)
		return &lc->info;

	fd = loopcxt_get_fd(lc);
	if (fd < 0)
		return NULL;

	if (ioctl(fd, LOOP_GET_STATUS64, &lc->info) == 0) {
		lc->has_info = 1;
		return &lc->info;
	}

	return NULL;
}

/*
 * @lc: context
 *
 * Returns (allocated) string with path to the file assicieted
 * with the current loop device.
 */
char *loopcxt_get_backing_file(struct loopdev_cxt *lc)
{
	struct sysfs_cxt *sysfs = loopcxt_get_sysfs(lc);
	char *res = NULL;

	if (sysfs)
		res = sysfs_strdup(sysfs, "loop/backing_file");

	if (!res && loopcxt_ioctl_enabled(lc)) {
		struct loop_info64 *lo = loopcxt_get_info(lc);

		if (lo) {
			lo->lo_file_name[LO_NAME_SIZE - 2] = '*';
			lo->lo_file_name[LO_NAME_SIZE - 1] = '\0';
			res = strdup((char *) lo->lo_file_name);
		}
	}
	return res;
}

/*
 * @lc: context
 * @offset: returns offset number for the given device
 *
 * Returns: <0 on error, 0 on success
 */
int loopcxt_get_offset(struct loopdev_cxt *lc, uint64_t *offset)
{
	struct sysfs_cxt *sysfs = loopcxt_get_sysfs(lc);
	int rc = -EINVAL;

	if (sysfs)
		rc = sysfs_read_u64(sysfs, "loop/offset", offset);

	if (rc && loopcxt_ioctl_enabled(lc)) {
		struct loop_info64 *lo = loopcxt_get_info(lc);
		if (lo) {
			if (offset)
				*offset = lo->lo_offset;
			return 0;
		}
	}

	return rc;
}

/*
 * @lc: context
 * @sizelimit: returns size limit for the given device
 *
 * Returns: <0 on error, 0 on success
 */
int loopcxt_get_sizelimit(struct loopdev_cxt *lc, uint64_t *size)
{
	struct sysfs_cxt *sysfs = loopcxt_get_sysfs(lc);
	int rc = -EINVAL;

	if (sysfs)
		rc = sysfs_read_u64(sysfs, "loop/sizelimit", size);

	if (rc && loopcxt_ioctl_enabled(lc)) {
		struct loop_info64 *lo = loopcxt_get_info(lc);
		if (lo) {
			if (size)
				*size = lo->lo_sizelimit;
			return 0;
		}
	}

	return rc;
}

/*
 * @lc: context
 *
 * Returns: 1 of the autoclear flags is set.
 */
int loopcxt_is_autoclear(struct loopdev_cxt *lc)
{
	struct sysfs_cxt *sysfs = loopcxt_get_sysfs(lc);

	if (sysfs) {
		int fl;
		if (sysfs_read_int(sysfs, "loop/autoclear", &fl) == 0)
			return fl;
	}

	if (loopcxt_ioctl_enabled(lc)) {
		struct loop_info64 *lo = loopcxt_get_info(lc);
		if (lo)
			return lo->lo_flags & LO_FLAGS_AUTOCLEAR;
	}
	return 0;
}

int loopcxt_set_offset(struct loopdev_cxt *lc, uint64_t offset)
{
	if (!lc)
		return -EINVAL;
	lc->info.lo_offset = offset;
	return 0;
}

int loopcxt_set_sizelimit(struct loopdev_cxt *lc, uint64_t sizelimit)
{
	if (!lc)
		return -EINVAL;
	lc->info.lo_sizelimit = sizelimit;
	return 0;
}

/*
 * @lc: context
 * @flags: kernel LO_FLAGS_{READ_ONLY,USE_AOPS,AUTOCLEAR} flags
 *
 * Returns: 0 on success, <0 on error.
 */
int loopcxt_set_flags(struct loopdev_cxt *lc, uint32_t flags)
{
	if (!lc)
		return -EINVAL;
	lc->info.lo_flags = flags;
	return 0;
}

/*
 * @lc: context
 * @filename: backing file path (the path will be canonicalized)
 *
 * Returns: 0 on success, <0 on error.
 */
int loopcxt_set_backing_file(struct loopdev_cxt *lc, const char *filename)
{
	if (!lc)
		return -EINVAL;

	lc->filename = canonicalize_path(filename);
	if (!lc->filename)
		return -errno;

	strncpy((char *)lc->info.lo_file_name, lc->filename, LO_NAME_SIZE);
	lc->info.lo_file_name[LO_NAME_SIZE- 1] = '\0';

	return 0;
}

static int digits_only(const char *s)
{
	while (*s)
		if (!isdigit(*s++))
			return 0;
	return 1;
}

/*
 * @lc: context
 * @encryption: encryption name / type (see lopsetup man page)
 * @password
 *
 * Note that the encyption functionality is deprecated an unmaintained. Use
 * cryptsetup (it also supports AES-loops).
 *
 * Returns: 0 on success, <0 on error.
 */
int loopcxt_set_encryption(struct loopdev_cxt *lc,
			   const char *encryption,
			   const char *password)
{
	if (!lc)
		return -EINVAL;

	if (encryption && *encryption) {
		if (digits_only(encryption)) {
			lc->info.lo_encrypt_type = atoi(encryption);
		} else {
			lc->info.lo_encrypt_type = LO_CRYPT_CRYPTOAPI;
			snprintf((char *)lc->info.lo_crypt_name, LO_NAME_SIZE,
				 "%s", encryption);
		}
	}

	switch (lc->info.lo_encrypt_type) {
	case LO_CRYPT_NONE:
		lc->info.lo_encrypt_key_size = 0;
		break;
	default:
		memset(lc->info.lo_encrypt_key, 0, LO_KEY_SIZE);
		strncpy((char *)lc->info.lo_encrypt_key, password, LO_KEY_SIZE);
		lc->info.lo_encrypt_key[LO_KEY_SIZE - 1] = '\0';
		lc->info.lo_encrypt_key_size = LO_KEY_SIZE;
		break;
	}
	return 0;
}

/*
 * @cl: context
 *
 * Associate the current device (see loopcxt_{set,get}_device()) with
 * a file (see loopcxt_set_backing_file()).
 *
 * Default is open backing file and device in read-write mode, see
 * LOOPDEV_FL_{RDONLY,RDWR} and loopcxt_init(). The LO_FLAGS_READ_ONLY lo_flag
 * will be set automatically according to LOOPDEV_FL_ flags.
 *
 * Returns: <0 on error, 0 on success.
 */
int loopcxt_setup_device(struct loopdev_cxt *lc)
{
	int file_fd, dev_fd, mode, rc = -1;

	if (!lc || !*lc->device || !lc->filename)
		return -EINVAL;

	/*
	 * Open backing file and device
	 */
	mode = (lc->flags & LOOPDEV_FL_RDONLY) ? O_RDONLY : O_RDWR;

	if ((file_fd = open(lc->filename, mode)) < 0) {
		if (mode != O_RDONLY && (errno == EROFS || errno == EACCES))
			file_fd = open(lc->filename, mode = O_RDONLY);

		if (file_fd < 0)
			return -errno;
	}

	if (mode == O_RDONLY) {
		lc->flags |= LOOPDEV_FL_RDONLY;
		lc->info.lo_flags |= LO_FLAGS_READ_ONLY;
	} else
		lc->flags |= LOOPDEV_FL_RDWR;

	dev_fd = loopcxt_get_fd(lc);
	if (dev_fd < 0) {
		rc = -errno;
		goto err;
	}

	/*
	 * Set FD
	 */
	if (ioctl(dev_fd, LOOP_SET_FD, file_fd) < 0) {
		rc = -errno;
		goto err;
	}
	close(file_fd);
	file_fd = -1;

	if (ioctl(dev_fd, LOOP_SET_STATUS64, &lc->info))
		goto err;

	memset(&lc->info, 0, sizeof(lc->info));
	lc->has_info = 0;

	return 0;
err:
	if (file_fd >= 0)
		close(file_fd);
	if (dev_fd >= 0)
		ioctl(dev_fd, LOOP_CLR_FD, 0);

	return rc;
}

int loopcxt_delete_device(struct loopdev_cxt *lc)
{
	int fd = loopcxt_get_fd(lc);

	if (fd < 0)
		return -EINVAL;

	if (ioctl(fd, LOOP_CLR_FD, 0) < 0)
		return -errno;
	return 0;
}

int loopcxt_find_unused(struct loopdev_cxt *lc)
{
	int rc;

	rc = loopcxt_init_iterator(lc, LOOPITER_FL_FREE);
	if (rc)
		return rc;

	rc = loopcxt_next(lc);
	loopcxt_deinit_iterator(lc);

	return rc;
}



/*
 * Return: TRUE/FALSE
 */
int loopdev_is_autoclear(const char *device)
{
	struct loopdev_cxt lc;
	int rc;

	if (!device)
		return 0;

	loopcxt_init(&lc, 0);
	loopcxt_set_device(&lc, device);
	rc = loopcxt_is_autoclear(&lc);
	loopcxt_deinit(&lc);

	return rc;
}

char *loopdev_get_backing_file(const char *device)
{
	struct loopdev_cxt lc;
	char *res;

	if (!device)
		return NULL;

	loopcxt_init(&lc, 0);
	loopcxt_set_device(&lc, device);
	res = loopcxt_get_backing_file(&lc);
	loopcxt_deinit(&lc);

	return res;
}

/*
 * Returns: TRUE/FALSE
 */
int loopdev_is_used(const char *device, const char *filename,
		    uint64_t offset, int flags)
{
	struct loopdev_cxt lc;
	char *backing = NULL;
	int rc = 0;

	if (!device)
		return 0;

	loopcxt_init(&lc, 0);
	loopcxt_set_device(&lc, device);

	backing = loopcxt_get_backing_file(&lc);
	if (!backing)
		goto done;
	if (filename && strcmp(filename, backing) != 0)
		goto done;
	if (flags & LOOPDEV_FL_OFFSET) {
		uint64_t off;

		if (loopcxt_get_offset(&lc, &off) != 0 || off != offset)
			goto done;
	}

	rc = 1;
done:
	free(backing);
	loopcxt_deinit(&lc);

	return rc;
}

int loopdev_delete(const char *device)
{
	struct loopdev_cxt lc;
	int rc;

	loopcxt_init(&lc, 0);
	rc = loopcxt_set_device(&lc, device);
	if (!rc)
		rc = loopcxt_delete_device(&lc);
	loopcxt_deinit(&lc);
	return rc;
}

/*
 * Returns: 0 = success, < 0 error, 1 not found
 */
int loopcxt_find_by_backing_file(struct loopdev_cxt *lc, const char *filename,
				 uint64_t offset, int flags)
{
	int rc;

	if (!filename)
		return -EINVAL;

	rc = loopcxt_init_iterator(lc, LOOPITER_FL_USED);
	if (rc)
		return rc;

	while((rc = loopcxt_next(lc)) == 0) {
		char *backing = loopcxt_get_backing_file(lc);

		if (!backing || strcmp(backing, filename)) {
			free(backing);
			continue;
		}

		free(backing);

		if (flags & LOOPDEV_FL_OFFSET) {
			uint64_t off;
			if (loopcxt_get_offset(lc, &off) != 0 || offset != off)
				continue;
		}

		rc = 0;
		break;
	}

	loopcxt_deinit_iterator(lc);
	return rc;
}

/*
 * Returns allocated string with device name
 */
char *loopdev_find_by_backing_file(const char *filename, uint64_t offset, int flags)
{
	struct loopdev_cxt lc;
	char *res = NULL;

	if (!filename)
		return NULL;

	loopcxt_init(&lc, 0);
	if (loopcxt_find_by_backing_file(&lc, filename, offset, flags))
		res = loopcxt_strdup_device(&lc);
	loopcxt_deinit(&lc);

	return res;
}


#ifdef TEST_PROGRAM_LOOPDEV
#include <errno.h>
#include <err.h>
#include <stdlib.h>

static void test_loop_info(const char *device, int flags)
{
	struct loopdev_cxt lc;
	char *p;
	uint64_t u64;

	loopcxt_init(&lc, flags);
	if (loopcxt_set_device(&lc, device))
		err(EXIT_FAILURE, "failed to set device");

	p = loopcxt_get_backing_file(&lc);
	printf("\tBACKING FILE: %s\n", p);
	free(p);

	if (loopcxt_get_offset(&lc, &u64) == 0)
		printf("\tOFFSET: %jd\n", u64);

	if (loopcxt_get_sizelimit(&lc, &u64) == 0)
		printf("\tSIZE LIMIT: %jd\n", u64);

	printf("\tAUTOCLEAR: %s\n", loopcxt_is_autoclear(&lc) ? "YES" : "NOT");

	loopcxt_deinit(&lc);
}

static void test_loop_scan(int flags)
{
	struct loopdev_cxt lc;
	int rc;

	loopcxt_init(&lc, 0);

	if (loopcxt_init_iterator(&lc, flags))
		err(EXIT_FAILURE, "iterator initlization failed");

	while((rc = loopcxt_next(&lc)) == 0) {
		const char *device = loopcxt_get_device(&lc);

		if (flags & LOOPITER_FL_USED) {
			char *backing = loopcxt_get_backing_file(&lc);
			printf("\t%s: %s\n", device, backing);
			free(backing);
		} else
			printf("\t%s\n", device);
	}

	if (rc < 0)
		err(EXIT_FAILURE, "loopdevs scanning failed");

	loopcxt_deinit(&lc);
}

static int test_loop_setup(const char *filename, const char *device)
{
	struct loopdev_cxt lc;
	int rc = 0;

	loopcxt_init(&lc, 0);

	if (loopcxt_set_backing_file(&lc, filename))
		err(EXIT_FAILURE, "failed to set backing file");

	if (device) {
		rc = loopcxt_set_device(&lc, device);
		if (rc)
			err(EXIT_FAILURE, "failed to set device: %s", device);
	}

	do {
		if (!device) {
			rc = loopcxt_find_unused(&lc);
			if (rc)
				err(EXIT_FAILURE, "failed to found unused device");
			printf("Trying to use '%s'\n", loopcxt_get_device(&lc));
		}

		rc = loopcxt_setup_device(&lc);
		if (rc == 0)
			break;		/* success */

		if (device || rc != -EBUSY)
			err(EXIT_FAILURE, "failed to setup device for %s",
					lc.filename);

		printf("device stolen...trying again\n");
	} while (1);

	loopcxt_deinit(&lc);

	return 0;
}

int main(int argc, char *argv[])
{
	if (argc < 2)
		goto usage;

	if (argc == 3 && strcmp(argv[1], "--info") == 0) {
		printf("---sysfs & ioctl:---\n");
		test_loop_info(argv[2], 0);
		printf("---sysfs only:---\n");
		test_loop_info(argv[2], LOOPDEV_FL_NOIOCTL);
		printf("---ioctl only:---\n");
		test_loop_info(argv[2], LOOPDEV_FL_NOSYSFS);

	} else if (argc == 2 && strcmp(argv[1], "--used") == 0) {
		printf("---all used devices---\n");
		test_loop_scan(LOOPITER_FL_USED);

	} else if (argc == 2 && strcmp(argv[1], "--free") == 0) {
		printf("---all free devices---\n");
		test_loop_scan(LOOPITER_FL_FREE);

	} else if (argc >= 3 && strcmp(argv[1], "--setup") == 0) {
		test_loop_setup(argv[2], argv[3]);

	} else if (argc == 3 && strcmp(argv[1], "--delete") == 0) {
		if (loopdev_delete(argv[2]))
			errx(EXIT_FAILURE, "failed to deinitialize device %s", argv[2]);
	} else
		goto usage;

	return EXIT_SUCCESS;

usage:
	errx(EXIT_FAILURE, "usage: \n"
			   "  %1$s --info <device>\n"
			   "  %1$s --free\n"
			   "  %1$s --used\n"
			   "  %1$s --setup <filename> [<device>]\n"
			   "  %1$s --delete\n",
			   argv[0]);
}

#endif /* TEST_PROGRAM */
