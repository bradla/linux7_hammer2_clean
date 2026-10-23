#include "hammer2.h"

int
cmd_bulkfree(const char *sel_path)
{
	hammer2_ioc_bulkfree_t bfi;
	int ecode = 0;
	int fd;
	int res;
	size_t usermem;
	size_t usermem_size = sizeof(usermem);

	bzero(&bfi, sizeof(bfi));
	usermem = 0;
	if (sysctlbyname("hw.usermem", &usermem, &usermem_size, NULL, 0) == 0)
		bfi.size = usermem / 16;
	else
		bfi.size = 0;
	if (bfi.size < 8192 * 1024)
		bfi.size = 8192 * 1024;

	if (MemOpt)
		bfi.size = (MemOpt + 8192 * 1024 - 1) & ~(8192 * 1024 - 1L);

	if ((fd = hammer2_ioctl_handle(sel_path)) < 0)
		return 1;
	res = ioctl(fd, HAMMER2IOC_BULKFREE_SCAN, &bfi);
	if (res) {
		perror("ioctl");
		ecode = 1;
	}
	return ecode;
}

#if 0
int
cmd_bulkfree_async(const char *sel_path)
{
	hammer2_ioc_bulkfree_t bfi;
	int ecode = 0;
	int fd;
	int res;

	bzero(&bfi, sizeof(bfi));
	bfi.size = 8192 * 1024;

	if ((fd = hammer2_ioctl_handle(sel_path)) < 0)
		return 1;
	res = ioctl(fd, HAMMER2IOC_BULKFREE_ASYNC, &bfi);
	if (res) {
		perror("ioctl");
		ecode = 1;
	}
	return ecode;
}
#endif
