#include <sys/param.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <err.h>
#include <errno.h>
#include <libutil.h>
#include <assert.h>

#define EDOOFUS EINVAL

struct pidfh {
	int	pf_fd;
	char	pf_path[MAXPATHLEN + 1];
	dev_t	pf_dev;
	ino_t	pf_ino;
};

static int _pidfile_remove(struct pidfh *pfh, int freeit);

static int
pidfile_verify(const struct pidfh *pfh)
{
	struct stat sb;

	if (pfh == NULL || pfh->pf_fd == -1)
		return (EDOOFUS);

	if (fstat(pfh->pf_fd, &sb) == -1)
		return (errno);
	if (sb.st_dev != pfh->pf_dev || sb.st_ino != pfh->pf_ino)
		return (EDOOFUS);
	return (0);
}

static int
pidfile_read(const char *path, pid_t *pidptr)
{
	char buf[16], *endptr;
	int error, fd, i;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd == -1)
		return (errno);

	i = read(fd, buf, sizeof(buf) - 1);
	error = errno;
	close(fd);
	if (i == -1)
		return (error);
	else if (i == 0)
		return (EAGAIN);
	buf[i] = '\0';

	*pidptr = strtol(buf, &endptr, 10);
	if (endptr != &buf[i])
		return (EINVAL);

	return (0);
}

struct pidfh *
pidfile_open(const char *path, mode_t mode, pid_t *pidptr)
{
	struct pidfh *pfh;
	struct stat sb;
	int error, fd, len, count;
	struct timespec rqtp;

	pfh = malloc(sizeof(*pfh));
	if (pfh == NULL)
		return (NULL);

	if (path == NULL)
		assert(0);
	else
		len = snprintf(pfh->pf_path, sizeof(pfh->pf_path),
		    "%s", path);
	if (len >= (int)sizeof(pfh->pf_path)) {
		free(pfh);
		errno = ENAMETOOLONG;
		return (NULL);
	}

	fd = flopen(pfh->pf_path,
	    O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NONBLOCK, mode);
	if (fd == -1) {
		if (errno == EWOULDBLOCK) {
			if (pidptr == NULL) {
				errno = EEXIST;
			} else {
				count = 20;
				rqtp.tv_sec = 0;
				rqtp.tv_nsec = 5000000;
				for (;;) {
					errno = pidfile_read(pfh->pf_path,
					    pidptr);
					if (errno != EAGAIN || --count == 0)
						break;
					nanosleep(&rqtp, 0);
				}
				if (errno == EAGAIN)
					*pidptr = -1;
				if (errno == 0 || errno == EAGAIN)
					errno = EEXIST;
			}
		}
		free(pfh);
		return (NULL);
	}

	if (fstat(fd, &sb) == -1) {
		error = errno;
		unlink(pfh->pf_path);
		close(fd);
		free(pfh);
		errno = error;
		return (NULL);
	}

	pfh->pf_fd = fd;
	pfh->pf_dev = sb.st_dev;
	pfh->pf_ino = sb.st_ino;

	return (pfh);
}

int
pidfile_write(struct pidfh *pfh)
{
	char pidstr[16];
	int error, fd;

	errno = pidfile_verify(pfh);
	if (errno != 0) {

		return (-1);
	}
	fd = pfh->pf_fd;

	if (ftruncate(fd, 0) == -1) {
		error = errno;
		_pidfile_remove(pfh, 0);
		errno = error;
		return (-1);
	}

	snprintf(pidstr, sizeof(pidstr), "%u", getpid());
	if (pwrite(fd, pidstr, strlen(pidstr), 0) != (ssize_t)strlen(pidstr)) {
		error = errno;
		_pidfile_remove(pfh, 0);
		errno = error;
		return (-1);
	}

	return (0);
}

int
pidfile_close(struct pidfh *pfh)
{
	int error;

	error = pidfile_verify(pfh);
	if (error != 0) {
		errno = error;
		return (-1);
	}

	if (close(pfh->pf_fd) == -1)
		error = errno;
	free(pfh);
	if (error != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

static int
_pidfile_remove(struct pidfh *pfh, int freeit)
{
	int error;

	error = pidfile_verify(pfh);
	if (error != 0) {
		errno = error;
		return (-1);
	}

	if (unlink(pfh->pf_path) == -1)
		error = errno;
	if (close(pfh->pf_fd) == -1) {
		if (error == 0)
			error = errno;
	}
	if (freeit)
		free(pfh);
	else
		pfh->pf_fd = -1;
	if (error != 0) {
		errno = error;
		return (-1);
	}
	return (0);
}

int
pidfile_remove(struct pidfh *pfh)
{

	return (_pidfile_remove(pfh, 1));
}

int
pidfile_fileno(const struct pidfh *pfh)
{

	if (pfh == NULL || pfh->pf_fd == -1) {
		errno = EDOOFUS;
		return (-1);
	}
	return (pfh->pf_fd);
}
