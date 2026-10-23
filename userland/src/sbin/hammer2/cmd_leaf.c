#include "hammer2.h"

int
cmd_leaf(const char *sel_info)
{
	int ecode = 0;
	int fd;

	if ((fd = hammer2_ioctl_handle(sel_info)) < 0)
		return(1);

	if (NormalExit)
		close(fd);

	return ecode;
}

#if 0

static
void *
leaf_connect(void *data)
{
	int fd;

	fd = (int)(intptr_t)data;

	return (NULL);
}
#endif
