#include "hammer2.h"

int
cmd_pfs_snapshot(const char *sel_path, const char *path, const char *label,
		 uint32_t pfs_flags)
{
	hammer2_ioc_pfs_t pfs;
	int ecode = 0;
	int fd;
	char filename[HAMMER2_INODE_MAXNAME + 16];
	time_t t;
	struct tm *tp;

	if (path == NULL) {
		fd = hammer2_ioctl_handle(sel_path);
	} else {
		fd = open(path, O_RDONLY);
		if (fd < 0)
			fprintf(stderr, "Unable to open %s\n", path);
	}
	if (fd < 0)
		return 1;

	if (label == NULL) {
		time(&t);
		tp = localtime(&t);
		bzero(&pfs, sizeof(pfs));
		pfs.name_key = (hammer2_key_t)-1;
		if (ioctl(fd, HAMMER2IOC_PFS_GET, &pfs) < 0) {
			perror("ioctl");
		}
		snprintf(filename, sizeof(filename),
			 "%s.%04d%02d%02d.%02d%02d%02d",
			 pfs.name,
			 tp->tm_year + 1900,
			 tp->tm_mon + 1,
			 tp->tm_mday,
			 tp->tm_hour,
			 tp->tm_min,
			 tp->tm_sec);
		label = filename;
	}

	bzero(&pfs, sizeof(pfs));
	snprintf(pfs.name, sizeof(pfs.name), "%s", label);
	pfs.pfs_flags = pfs_flags;

	if (ioctl(fd, HAMMER2IOC_PFS_SNAPSHOT, &pfs) < 0) {
		perror("ioctl");
		ecode = 1;
	} else {
		printf("created snapshot %s\n", label);
	}
	return ecode;
}
