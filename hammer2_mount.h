#ifndef _VFS_HAMMER2_MOUNT_H_
#define _VFS_HAMMER2_MOUNT_H_

struct hammer2_mount_info {
	const char	*volume;
	int		hflags;
	int		cluster_fd;
	char		reserved1[112];
};

#define HMNT2_UNUSED01		0x00000001
#define HMNT2_LOCAL		0x00000002
#define HMNT2_EMERG		0x00000004

#define HMNT2_USERFLAGS		(HMNT2_LOCAL)
#define HMNT2_DEVFLAGS		(HMNT2_LOCAL)

#endif
