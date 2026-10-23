#ifndef _HAMMER2_IOCTL_H_
#define _HAMMER2_IOCTL_H_

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/limits.h>

#include "hammer2_disk.h"

#define HAMMER2_PATH_MAX	PATH_MAX
#define HAMMER2_NAME_MAX	NAME_MAX

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

struct hammer2_ioc_pfs {
	u64			name_key;
	u64			name_next;
	u8			pfs_type;
	u8			pfs_subtype;
	u8			reserved0012;
	u8			reserved0013;
	u32			pfs_flags;
	u64			reserved0018;
	struct uuid		pfs_fsid;
	struct uuid		pfs_clid;
	char			name[NAME_MAX + 1];
};

typedef struct hammer2_ioc_pfs hammer2_ioc_pfs_t;

#define HAMMER2_PFSFLAGS_NOSYNC		0x00000001

struct hammer2_ioc_inode {
	u32			flags;
	void			*unused;
	u64			data_count;
	u64			inode_count;
	hammer2_inode_data_t	ip_data;
};

typedef struct hammer2_ioc_inode hammer2_ioc_inode_t;

#define HAMMER2IOC_INODE_FLAG_IQUOTA	0x00000001
#define HAMMER2IOC_INODE_FLAG_DQUOTA	0x00000002
#define HAMMER2IOC_INODE_FLAG_COPIES	0x00000004
#define HAMMER2IOC_INODE_FLAG_CHECK	0x00000008
#define HAMMER2IOC_INODE_FLAG_COMP	0x00000010

struct hammer2_ioc_bulkfree {
	u64			sbase;
	u64			sstop;
	size_t			size;
	u64			count_allocated;
	u64			count_freed;
	u64			total_fragmented;
	u64			total_allocated;
	u64			total_scanned;
};

typedef struct hammer2_ioc_bulkfree hammer2_ioc_bulkfree_t;

#define HAMMER2_INODE_MAXNAME	256

struct hammer2_ioc_destroy {
	enum {
		HAMMER2_DELETE_NOP,
		HAMMER2_DELETE_FILE,
		HAMMER2_DELETE_INUM
	} cmd;
	char			path[HAMMER2_INODE_MAXNAME];
	u64			inum;
};

typedef struct hammer2_ioc_destroy hammer2_ioc_destroy_t;

struct hammer2_ioc_growfs {
	u64			size;
	int			modified;
	int			unused01;
	int			unusedary[14];
};

typedef struct hammer2_ioc_growfs hammer2_ioc_growfs_t;

struct hammer2_ioc_volume {
	char			path[HAMMER2_PATH_MAX];
	int			id;
	u64			offset;
	u64			size;
};

typedef struct hammer2_ioc_volume hammer2_ioc_volume_t;

struct hammer2_ioc_volume_list {
	struct hammer2_ioc_volume __user *volumes;
	int			nvolumes;
	int			version;
	char			pfs_name[HAMMER2_INODE_MAXNAME];
};

typedef struct hammer2_ioc_volume_list hammer2_ioc_volume_list_t;

struct hammer2_ioc_volume2 {
	char			path[64];
	int			id;
	u64			offset;
	u64			size;
};

typedef struct hammer2_ioc_volume2 hammer2_ioc_volume2_t;

struct hammer2_ioc_volume_list2 {
	struct hammer2_ioc_volume2	volumes[HAMMER2_MAX_VOLUMES];
	int			nvolumes;
	int			version;
	char			pfs_name[HAMMER2_INODE_MAXNAME];
};

typedef struct hammer2_ioc_volume_list2 hammer2_ioc_volume_list2_t;

#define HAMMER2_IOC(type, nr, struct) \
	_IOC(_IOC_READ|_IOC_WRITE, type, nr, sizeof(struct))

#define HAMMER2_IOR(type, nr, struct) \
	_IOC(_IOC_READ, type, nr, sizeof(struct))

#define HAMMER2_IOW(type, nr, struct) \
	_IOC(_IOC_WRITE, type, nr, sizeof(struct))

#define HAMMER2_IOWR(type, nr, struct) \
	_IOC(_IOC_READ|_IOC_WRITE, type, nr, sizeof(struct))

#define HAMMER2IOC_VERSION_GET		HAMMER2_IOWR('h', 64, struct hammer2_ioc_version)
#define HAMMER2IOC_RECLUSTER		HAMMER2_IOWR('h', 65, struct hammer2_ioc_recluster)
#define HAMMER2IOC_PFS_GET		HAMMER2_IOWR('h', 80, struct hammer2_ioc_pfs)
#define HAMMER2IOC_PFS_CREATE		HAMMER2_IOWR('h', 81, struct hammer2_ioc_pfs)
#define HAMMER2IOC_PFS_DELETE		HAMMER2_IOWR('h', 82, struct hammer2_ioc_pfs)
#define HAMMER2IOC_PFS_LOOKUP		HAMMER2_IOWR('h', 83, struct hammer2_ioc_pfs)
#define HAMMER2IOC_PFS_SNAPSHOT		HAMMER2_IOWR('h', 84, struct hammer2_ioc_pfs)
#define HAMMER2IOC_INODE_GET		HAMMER2_IOWR('h', 86, struct hammer2_ioc_inode)
#define HAMMER2IOC_INODE_SET		HAMMER2_IOWR('h', 87, struct hammer2_ioc_inode)
#define HAMMER2IOC_DEBUG_DUMP		HAMMER2_IOWR('h', 91, int)
#define HAMMER2IOC_BULKFREE_SCAN	HAMMER2_IOWR('h', 92, struct hammer2_ioc_bulkfree)
#define HAMMER2IOC_DESTROY		HAMMER2_IOWR('h', 94, struct hammer2_ioc_destroy)
#define HAMMER2IOC_EMERG_MODE		HAMMER2_IOWR('h', 95, int)
#define HAMMER2IOC_GROWFS		HAMMER2_IOWR('h', 96, struct hammer2_ioc_growfs)
#define HAMMER2IOC_VOLUME_LIST		HAMMER2_IOWR('h', 97, struct hammer2_ioc_volume_list)
#define HAMMER2IOC_VOLUME_LIST2		HAMMER2_IOWR('h', 197, struct hammer2_ioc_volume_list2)

#define HAMMER2IOC_PFS_GET_OLD		HAMMER2IOC_PFS_GET
#define HAMMER2IOC_PFS_CREATE_OLD	HAMMER2IOC_PFS_CREATE
#define HAMMER2IOC_PFS_DELETE_OLD	HAMMER2IOC_PFS_DELETE
#define HAMMER2IOC_PFS_LOOKUP_OLD	HAMMER2IOC_PFS_LOOKUP
#define HAMMER2IOC_PFS_SNAPSHOT_OLD	HAMMER2IOC_PFS_SNAPSHOT

#endif
