#ifndef _VFS_HAMMER2_IOCTL_H_
#define _VFS_HAMMER2_IOCTL_H_

#include <sys/param.h>
#include <sys/syslimits.h>

#ifndef _SYS_IOCCOM_H_
#include <sys/ioccom.h>
#endif
#ifndef _VFS_HAMMER2_DISK_H_
#include "hammer2_disk.h"
#endif
#ifndef _VFS_HAMMER2_MOUNT_H_
#include "hammer2_mount.h"
#endif

struct hammer2_ioc_version {
	int			version;
	char			reserved[256 - 4];
};

typedef struct hammer2_ioc_version hammer2_ioc_version_t;

struct hammer2_ioc_recluster {
	int			fd;
	char			reserved[256 - 4];
};

typedef struct hammer2_ioc_recluster hammer2_ioc_recluster_t;

struct hammer2_ioc_remote {
	int			copyid;
	int			nextid;
	int			fd;
	int			reserved03;
	int			reserved04[8];
	hammer2_volconf_t	copy1;
	hammer2_volconf_t	copy2;
};

typedef struct hammer2_ioc_remote hammer2_ioc_remote_t;

struct hammer2_ioc_pfs {
	hammer2_key_t		name_key;
	hammer2_key_t		name_next;
	uint8_t			pfs_type;
	uint8_t			pfs_subtype;
	uint8_t			reserved0012;
	uint8_t			reserved0013;
	uint32_t		pfs_flags;
	uint64_t		reserved0018;
	uuid_t			pfs_fsid;
	uuid_t			pfs_clid;
	char			name[NAME_MAX+1];
};

typedef struct hammer2_ioc_pfs hammer2_ioc_pfs_t;

#define HAMMER2_PFSFLAGS_NOSYNC		0x00000001

struct hammer2_ioc_inode {
	uint32_t		flags;
	void			*unused;
	hammer2_key_t		data_count;
	hammer2_key_t		inode_count;
	hammer2_inode_data_t	ip_data;
};

typedef struct hammer2_ioc_inode hammer2_ioc_inode_t;

#define HAMMER2IOC_INODE_FLAG_IQUOTA	0x00000001
#define HAMMER2IOC_INODE_FLAG_DQUOTA	0x00000002
#define HAMMER2IOC_INODE_FLAG_COPIES	0x00000004
#define HAMMER2IOC_INODE_FLAG_CHECK	0x00000008
#define HAMMER2IOC_INODE_FLAG_COMP	0x00000010

struct hammer2_ioc_bulkfree {
	hammer2_off_t		sbase;
	hammer2_off_t		sstop;
	size_t			size;
	hammer2_off_t		count_allocated;
	hammer2_off_t		count_freed;
	hammer2_off_t		total_fragmented;
	hammer2_off_t		total_allocated;
	hammer2_off_t		total_scanned;
};

typedef struct hammer2_ioc_bulkfree hammer2_ioc_bulkfree_t;

struct hammer2_ioc_destroy {
	enum { HAMMER2_DELETE_NOP,
	       HAMMER2_DELETE_FILE,
	       HAMMER2_DELETE_INUM } cmd;
	char			path[HAMMER2_INODE_MAXNAME];
	hammer2_key_t		inum;
};

typedef struct hammer2_ioc_destroy hammer2_ioc_destroy_t;

struct hammer2_ioc_growfs {
	hammer2_off_t		size;
	int			modified;
	int			unused01;
	int			unusedary[14];
};

typedef struct hammer2_ioc_growfs hammer2_ioc_growfs_t;

struct hammer2_ioc_volume {
	char			path[MAXPATHLEN];
	int			id;
	hammer2_off_t		offset;
	hammer2_off_t		size;
};

typedef struct hammer2_ioc_volume hammer2_ioc_volume_t;

struct hammer2_ioc_volume_list {
	hammer2_ioc_volume_t	*volumes;
	int			nvolumes;
	int			version;
	char			pfs_name[HAMMER2_INODE_MAXNAME];
};

typedef struct hammer2_ioc_volume_list hammer2_ioc_volume_list_t;

#define HAMMER2IOC_VERSION_GET	_IOWR('h', 64, struct hammer2_ioc_version)
#define HAMMER2IOC_RECLUSTER	_IOWR('h', 65, struct hammer2_ioc_recluster)

#define HAMMER2IOC_REMOTE_SCAN	_IOWR('h', 68, struct hammer2_ioc_remote)
#define HAMMER2IOC_REMOTE_ADD	_IOWR('h', 69, struct hammer2_ioc_remote)
#define HAMMER2IOC_REMOTE_DEL	_IOWR('h', 70, struct hammer2_ioc_remote)
#define HAMMER2IOC_REMOTE_REP	_IOWR('h', 71, struct hammer2_ioc_remote)

#define HAMMER2IOC_SOCKET_GET	_IOWR('h', 76, struct hammer2_ioc_remote)
#define HAMMER2IOC_SOCKET_SET	_IOWR('h', 77, struct hammer2_ioc_remote)

#define HAMMER2IOC_PFS_GET	_IOWR('h', 80, struct hammer2_ioc_pfs)
#define HAMMER2IOC_PFS_CREATE	_IOWR('h', 81, struct hammer2_ioc_pfs)
#define HAMMER2IOC_PFS_DELETE	_IOWR('h', 82, struct hammer2_ioc_pfs)
#define HAMMER2IOC_PFS_LOOKUP	_IOWR('h', 83, struct hammer2_ioc_pfs)
#define HAMMER2IOC_PFS_SNAPSHOT	_IOWR('h', 84, struct hammer2_ioc_pfs)

#define HAMMER2IOC_INODE_GET	_IOWR('h', 86, struct hammer2_ioc_inode)
#define HAMMER2IOC_INODE_SET	_IOWR('h', 87, struct hammer2_ioc_inode)

#define HAMMER2IOC_DEBUG_DUMP	_IOWR('h', 91, int)
#define HAMMER2IOC_BULKFREE_SCAN _IOWR('h', 92, struct hammer2_ioc_bulkfree)
#define HAMMER2IOC_BULKFREE_ASYNC _IOWR('h', 93, struct hammer2_ioc_bulkfree)
#define HAMMER2IOC_DESTROY	_IOWR('h', 94, struct hammer2_ioc_destroy)
#define HAMMER2IOC_EMERG_MODE	_IOWR('h', 95, int)
#define HAMMER2IOC_GROWFS	_IOWR('h', 96, struct hammer2_ioc_growfs)
#define HAMMER2IOC_VOLUME_LIST	_IOWR('h', 97, struct hammer2_ioc_volume_list)

#endif
