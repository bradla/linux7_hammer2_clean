#ifndef _FS_HAMMER2_MOUNT_H_
#define _FS_HAMMER2_MOUNT_H_

struct hammer2_mount_info {
	char		*fspec;
	int		hflags;
};

#define HMNT2_LOCAL		0x00000002
#define HMNT2_EMERG		0x00000004

#define HMNT2_DEVFLAGS		(HMNT2_LOCAL)

#endif
