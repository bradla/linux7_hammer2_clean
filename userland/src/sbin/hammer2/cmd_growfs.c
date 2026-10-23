#include "hammer2.h"

int
cmd_growfs(const char *sel_path, int ac, const char **av)
{
	int fd;
	int i;
	int ecode = 0;

	for (i = 0; i <= ac; ++i) {
		struct hammer2_ioc_growfs growfs;

		if (i < ac)
			sel_path = av[i];
		else if (i == ac && ac != 0)
			continue;

		fd = hammer2_ioctl_handle(sel_path);
		if (fd < 0) {
			ecode = 1;
			continue;
		}
		bzero(&growfs, sizeof(growfs));
		if (ioctl(fd, HAMMER2IOC_GROWFS, &growfs) < 0) {
			fprintf(stderr, "grow %s failed: %s\n",
			       sel_path, strerror(errno));
			ecode = 1;
		} else if (growfs.modified) {
			printf("%s grown to %ld\n",
			       sel_path, (intmax_t)growfs.size);
		} else {
			printf("%s no size change - %ld\n",
			       sel_path, (intmax_t)growfs.size);
		}
		close(fd);
	}
	return ecode;
}
