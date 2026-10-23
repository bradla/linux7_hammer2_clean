#include <sys/stat.h>
#include <sys/file.h>

#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <libutil.h>

int
flopen(const char *path, int flags, ...)
{
	int fd, operation, serrno, trunc;
	struct stat sb, fsb;
	mode_t mode;

#ifdef O_EXLOCK
	flags &= ~O_EXLOCK;
#endif

	mode = 0;
	if (flags & O_CREAT) {
		va_list ap;

		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}

        operation = LOCK_EX;
        if (flags & O_NONBLOCK)
                operation |= LOCK_NB;

	trunc = (flags & O_TRUNC);
	flags &= ~O_TRUNC;

	for (;;) {
		if ((fd = open(path, flags, mode)) == -1)

			return (-1);
		if (flock(fd, operation) == -1) {

			serrno = errno;
			(void)close(fd);
			errno = serrno;
			return (-1);
		}
		if (stat(path, &sb) == -1) {

			(void)close(fd);
			continue;
		}
		if (fstat(fd, &fsb) == -1) {

			serrno = errno;
			(void)close(fd);
			errno = serrno;
			return (-1);
		}
		if (sb.st_dev != fsb.st_dev ||
		    sb.st_ino != fsb.st_ino) {

			(void)close(fd);
			continue;
		}
		if (trunc && ftruncate(fd, 0) != 0) {

			serrno = errno;
			(void)close(fd);
			errno = serrno;
			return (-1);
		}
		return (fd);
	}
}
