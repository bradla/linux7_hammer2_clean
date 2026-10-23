#include "hammer2.h"

int cmd_emergency_mode(const char *sel_path __unused, int enable,
		       int ac, const char **av)
{
	int error = 0;
	int fd;

	if (ac != 1) {
		fprintf(stderr, "Expected <filesystem> argument\n");
		return 1;
	}
	fd = open(av[0], O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Unable to find \"%s\"\n", av[0]);
		return 1;
	}
	if (ioctl(fd, HAMMER2IOC_EMERG_MODE, &enable) == 0) {
		if (enable)
			printf("Emergency mode on \"%s\" enabled\n", av[0]);
		else
			printf("Emergency mode on \"%s\" disabled\n", av[0]);
	} else {
		printf("Cannot change emerg mode: %s\n", strerror(errno));
		error = 1;
	}
	close(fd);

	return error;
}
