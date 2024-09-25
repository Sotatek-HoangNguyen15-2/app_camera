#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <assert.h>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <cctype>
#include <linux/videodev2.h>

#include "utils.h"

static int
os_fd_set_cloexec(int fd)
{
	long flags;

	if (fd == -1)
		return -1;

	flags = fcntl(fd, F_GETFD);
	if (flags == -1)
		return -1;

	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1)
		return -1;

	return 0;
}

static int
set_cloexec_or_close(int fd)
{
	if (os_fd_set_cloexec(fd) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
create_tmpfile_cloexec(char *tmpname)
{
	int fd;

#ifdef HAVE_MKOSTEMP
	fd = mkostemp(tmpname, O_CLOEXEC);
	if (fd >= 0)
		unlink(tmpname);
#else
	fd = mkstemp(tmpname);
	if (fd >= 0) {
		fd = set_cloexec_or_close(fd);
		unlink(tmpname);
	}
#endif

	return fd;
}

/*
 * Create a new, unique, anonymous file of the given size, and
 * return the file descriptor for it. The file descriptor is set
 * CLOEXEC. The file is immediately suitable for mmap()'ing
 * the given size at offset zero.
 *
 * The file should not have a permanent backing store like a disk,
 * but may have if XDG_RUNTIME_DIR is not properly implemented in OS.
 *
 * The file name is deleted from the file system.
 *
 * The file is suitable for buffer sharing between processes by
 * transmitting the file descriptor over Unix sockets using the
 * SCM_RIGHTS methods.
 *
 * If the C library implements posix_fallocate(), it is used to
 * guarantee that disk space is available for the file at the
 * given size. If disk space is insufficient, errno is set to ENOSPC.
 * If posix_fallocate() is not supported, program may receive
 * SIGBUS on accessing mmap()'ed file contents instead.
 *
 * If the C library implements memfd_create(), it is used to create the
 * file purely in memory, without any backing file name on the file
 * system, and then sealing off the possibility of shrinking it.  This
 * can then be checked before accessing mmap()'ed file contents, to
 * make sure SIGBUS can't happen.  It also avoids requiring
 * XDG_RUNTIME_DIR.
 */
int
os_create_anonymous_file(off_t size)
{
	static const char weston_template[] = "/weston-shared-XXXXXX";
	const char *path;
	char *name;
	int fd;
	int ret;

#ifdef HAVE_MEMFD_CREATE
	fd = memfd_create("weston-shared", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd >= 0) {
		/* We can add this seal before calling posix_fallocate(), as
		 * the file is currently zero-sized anyway.
		 *
		 * There is also no need to check for the return value, we
		 * couldn't do anything with it anyway.
		 */
		fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK);
	} else
#endif
	{
		path = getenv("XDG_RUNTIME_DIR");
		if (!path) {
			errno = ENOENT;
			return -1;
		}

		name = static_cast<char *>(malloc(strlen(path) + sizeof(weston_template)));
		if (!name)
			return -1;

		strcpy(name, path);
		strcat(name, weston_template);

		fd = create_tmpfile_cloexec(name);

		free(name);

		if (fd < 0)
			return -1;
	}

#ifdef HAVE_POSIX_FALLOCATE
	do {
		ret = posix_fallocate(fd, 0, size);
	} while (ret == EINTR);
	if (ret != 0) {
		close(fd);
		errno = ret;
		return -1;
	}
#else
	do {
		ret = ftruncate(fd, size);
	} while (ret < 0 && errno == EINTR);
	if (ret < 0) {
		close(fd);
		return -1;
	}
#endif

	return fd;
}

const char*
get_first_camera_device(void)
{
	DIR *dir = opendir("/dev");
	if (!dir) {
		perror("Couldn't open the '/dev' directory");
		return NULL;
	}

	static char device[PATH_MAX];
	struct dirent *dirent;
	bool found = false;
	while ((dirent = readdir(dir)) != NULL) {
		struct v4l2_capability vid_cap;
		int fd;

		if (strcmp(dirent->d_name, "video"))
			continue;
		if (!isdigit(dirent->d_name[5]))
			continue;

		snprintf(device, sizeof(device), "/dev/%s", dirent->d_name);

		fd = open(device, O_RDWR);
		if (fd == -1)
			continue;
		if (ioctl(fd, VIDIOC_QUERYCAP, &vid_cap) < 0) {
			close(fd);
			continue;
		}
		close(fd);

		if ((vid_cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
			(vid_cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
			found = true;
			break;
		}
	}

	closedir(dir);
	return found ? device : NULL;
}
