#include "hammer2.h"

static int pathdir(const char *path, const char **lastp);

int
cmd_destroy_path(int ac, const char **av)
{
	hammer2_ioc_destroy_t destroy;
	const char *last;
	int i;
	int fd;
	int ecode = 0;

	for (i = 0; i < ac; ++i) {
		bzero(&destroy, sizeof(destroy));
		destroy.cmd = HAMMER2_DELETE_FILE;
		printf("%s\t", av[i]);
		fflush(stdout);
		fd = pathdir(av[i], &last);
		if (fd >= 0) {
			snprintf(destroy.path, sizeof(destroy.path),
				 "%s", last);
			if (ioctl(fd, HAMMER2IOC_DESTROY, &destroy) < 0) {
				printf("%s\n", strerror(errno));
				ecode = 1;
			} else {
				printf("ok\n");
			}
			close(fd);
		} else {
			printf("%s\n", strerror(errno));
			ecode = 1;
		}
	}
	return ecode;
}

static
int
pathdir(const char *path, const char **lastp)
{
	const char *ptr;
	char *npath;
	int fd;

	ptr = path + strlen(path);
	while (ptr > path && ptr[-1] != '/')
		--ptr;
	*lastp = ptr;
	if (ptr == path) {
		fd = open(".", O_RDONLY);
	} else {
		asprintf(&npath, "%*.*s",
			(int)(ptr - path), (int)(ptr - path), path);
		fd = open(npath, O_RDONLY);
		free(npath);
	}

	return fd;
}

int
cmd_destroy_inum(const char *sel_path, int ac, const char **av)
{
	hammer2_ioc_destroy_t destroy;
	int i;
	int fd;
	int ecode = 0;

	fd = hammer2_ioctl_handle(sel_path);
	if (fd < 0)
		return 1;

	printf("deleting inodes on %s\n", sel_path);
	for (i = 0; i < ac; ++i) {
		bzero(&destroy, sizeof(destroy));
		destroy.cmd = HAMMER2_DELETE_INUM;
		destroy.inum = strtoul(av[i], NULL, 0);
		printf("%16jd ", (intmax_t)destroy.inum);
		if (ioctl(fd, HAMMER2IOC_DESTROY, &destroy) < 0) {
			printf("%s\n", strerror(errno));
			ecode = 1;
		} else {
			printf("ok\n");
		}
	}
	close(fd);

	return ecode;
}
