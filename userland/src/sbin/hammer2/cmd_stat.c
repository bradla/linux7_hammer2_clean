#include "hammer2.h"

static const char *compmodestr(uint8_t comp_algo);
static const char *checkmodestr(uint8_t comp_algo);

int
cmd_stat(int ac, const char **av)
{
	hammer2_ioc_inode_t ino;
	const char *cdir = ".";
	int ecode = 0;
	int w;
	int i;
	int fd;

	if (ac == 0) {
		ac = 1;
		av = &cdir;
	}
	for (i = w = 0; i < ac; ++i) {
		if (w < (int)strlen(av[i]))
			w = (int)strlen(av[i]);
	}
	if (w < 16)
		w = 16;
	printf("%-*.*s ncp  data-use inode-use comp               check        quota\n",
		w, w, "PATH");
	for (i = 0; i < ac; ++i) {
		if ((fd = open(av[i], O_RDONLY)) < 0) {
			fprintf(stderr, "%s: %s\n", av[i], strerror(errno));
			ecode = 1;
			continue;
		}
		if (ioctl(fd, HAMMER2IOC_INODE_GET, &ino) < 0) {
			fprintf(stderr, "%s: %s\n", av[i], strerror(errno));
			ecode = 1;
			continue;
		}
		printf("%-*.*s ", w, w, av[i]);
		printf("%3d ", ino.ip_data.meta.ncopies);
		printf("%9s ", sizetostr(ino.data_count));
		printf("%9s ", counttostr(ino.inode_count));
		printf("%-18s ", compmodestr(ino.ip_data.meta.comp_algo));
		printf("%-12s ", checkmodestr(ino.ip_data.meta.check_algo));
		if (ino.ip_data.meta.data_quota ||
		    ino.ip_data.meta.inode_quota) {
			printf("%s",
				sizetostr(ino.ip_data.meta.data_quota));
			printf("/%-12s",
				counttostr(ino.ip_data.meta.inode_quota));
		}
		printf("\n");
	}
	return ecode;
}

static
const char *
compmodestr(uint8_t comp_algo)
{
	static char buf[64];
	static const char *comps[] = HAMMER2_COMP_STRINGS;
	int comp = HAMMER2_DEC_ALGO(comp_algo);
	int level = HAMMER2_DEC_LEVEL(comp_algo);

	if (level) {
		if (comp >= 0 && comp < HAMMER2_COMP_STRINGS_COUNT)
			snprintf(buf, sizeof(buf), "%s:%d",
				 comps[comp], level);
		else
			snprintf(buf, sizeof(buf), "unknown(%d):%d",
				 comp, level);
	} else {
		if (comp >= 0 && comp < HAMMER2_COMP_STRINGS_COUNT)
			snprintf(buf, sizeof(buf), "%s:default",
				 comps[comp]);
		else
			snprintf(buf, sizeof(buf), "unknown(%d):default",
				 comp);
	}
	return (buf);
}

static
const char *
checkmodestr(uint8_t check_algo)
{
	static char buf[64];
	static const char *checks[] = HAMMER2_CHECK_STRINGS;
	int check = HAMMER2_DEC_ALGO(check_algo);
	int level = HAMMER2_DEC_LEVEL(check_algo);

	if (level) {
		if (check >= 0 && check < HAMMER2_CHECK_STRINGS_COUNT)
			snprintf(buf, sizeof(buf), "%s:%d",
				 checks[check], level);
		else
			snprintf(buf, sizeof(buf), "unknown(%d):%d",
				 check, level);
	} else {
		if (check >= 0 && check < HAMMER2_CHECK_STRINGS_COUNT)
			snprintf(buf, sizeof(buf), "%s", checks[check]);
		else
			snprintf(buf, sizeof(buf), "unknown(%d)", check);
	}
	return (buf);
}
