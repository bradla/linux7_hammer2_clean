#include "hammer2.h"

int
cmd_remote_connect(const char *sel_path, const char *url)
{
	hammer2_ioc_remote_t remote;
	int ecode = 0;
	int fd;

	if ((fd = hammer2_ioctl_handle(sel_path)) < 0)
		return(1);
	bzero(&remote, sizeof(remote));
	remote.copyid = -1;
	remote.fd = -1;
	if (strlen(url) >= sizeof(remote.copy1.path)) {
		fprintf(stderr, "hammer2: connect: Path too long\n");
		close(fd);
		return(1);
	}
	snprintf((char*)remote.copy1.path, sizeof(remote.copy1.path), "%s",
		 url);
	if (ioctl(fd, HAMMER2IOC_REMOTE_ADD, &remote) < 0) {
		perror("ioctl");
		ecode = 1;
	}
	close(fd);
	return ecode;
}

int
cmd_remote_disconnect(const char *sel_path, const char *url)
{
	hammer2_ioc_remote_t remote;
	int ecode = 0;
	int fd;

	if ((fd = hammer2_ioctl_handle(sel_path)) < 0)
		return(1);
	bzero(&remote, sizeof(remote));
	remote.copyid = -1;
	remote.fd = -1;
	if (strlen(url) >= sizeof(remote.copy1.path)) {
		fprintf(stderr, "hammer2: disconnect: Path too long\n");
		close(fd);
		return(1);
	}
	snprintf((char*)remote.copy1.path, sizeof(remote.copy1.path), "%s",
		 url);
	if (ioctl(fd, HAMMER2IOC_REMOTE_DEL, &remote) < 0) {
		perror("ioctl");
		ecode = 1;
	}
	close(fd);
	return ecode;
}

int
cmd_remote_status(const char *sel_path, int all_opt __unused)
{
	hammer2_ioc_remote_t remote;
	int ecode = 0;
	int count = 0;
	int fd;

	if ((fd = hammer2_ioctl_handle(sel_path)) < 0)
		return(1);
	bzero(&remote, sizeof(remote));

	while ((remote.copyid = remote.nextid) >= 0) {
		if (ioctl(fd, HAMMER2IOC_REMOTE_SCAN, &remote) < 0) {
			perror("ioctl");
			ecode = 1;
			break;
		}
		if (remote.copy1.copyid == 0)
			continue;
		if (count == 0)
			printf("CPYID LABEL           STATUS PATH\n");
		printf("%5d %-15s %c%c%c.%02x %s\n",
			remote.copy1.copyid,
			remote.copy1.label,
			'-', '-', '-',
			remote.copy1.priority,
			remote.copy1.path);
		++count;
	}
	if (count == 0)
		printf("No linkages found\n");
	return (ecode);
}
