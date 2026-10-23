#include "hammer2.h"

static void
hexdump_inode(const void *data, size_t len)
{
	const unsigned char *p = data;
	size_t i;

	if (VerboseOpt <= 0)
		return;

	for (i = 0; i < len; i++) {
		printf("%02X", *p);
		if (i && !((i + 1) % 16))
			printf("\n");
		else if (i != len - 1)
			printf(" ");
		p++;
	}
	printf("\n");
}

void
print_inode(const char *path)
{
	hammer2_ioc_inode_t inode;
	hammer2_inode_data_t *ipdata;
	hammer2_inode_meta_t *meta;
	char *str = NULL;
	int fd;
	hammer2_uuid_t uuid;

	fd = hammer2_ioctl_handle(path);
	if (fd == -1)
		return;

	if (ioctl(fd, HAMMER2IOC_INODE_GET, &inode) == -1) {
		printf("ioctl(HAMMER2IOC_INODE_GET) failed\n");
		return;
	}
	ipdata = &inode.ip_data;
	meta = &ipdata->meta;

	hexdump_inode(meta, sizeof(*meta));

	printf("version = %u\n", meta->version);
	printf("pfs_subtype = %u (%s)\n", meta->pfs_subtype,
	    hammer2_pfssubtype_to_str(meta->pfs_subtype));
	printf("uflags = 0x%x\n", (unsigned int)meta->uflags);
	printf("rmajor = %u\n", meta->rmajor);
	printf("rminor = %u\n", meta->rminor);
	printf("ctime = %s\n", hammer2_time64_to_str(meta->ctime, &str));
	printf("mtime = %s\n", hammer2_time64_to_str(meta->mtime, &str));
	printf("atime = %s\n", hammer2_time64_to_str(meta->atime, &str));
	printf("btime = %s\n", hammer2_time64_to_str(meta->btime, &str));
	uuid = meta->uid;
	printf("uid = %s\n", hammer2_uuid_to_str(&uuid, &str));
	uuid = meta->gid;
	printf("gid = %s\n", hammer2_uuid_to_str(&uuid, &str));
	printf("type = %u (%s)\n", meta->type,
	    hammer2_iptype_to_str(meta->type));
	printf("op_flags = 0x%x\n", meta->op_flags);
	printf("cap_flags = 0x%x\n", meta->cap_flags);
	printf("mode = 0%o\n", meta->mode);
	printf("inum = 0x%jx\n", (uintmax_t)meta->inum);
	printf("size = %ju\n", (uintmax_t)meta->size);
	printf("nlinks = %ju\n", (uintmax_t)meta->nlinks);
	printf("iparent = 0x%jx\n", (uintmax_t)meta->iparent);
	printf("name_key = 0x%jx\n", (uintmax_t)meta->name_key);
	printf("name_len = %u\n", meta->name_len);
	printf("ncopies = %u\n", meta->ncopies);
	printf("comp_algo = %u\n", meta->comp_algo);
	printf("target_type = %u\n", meta->target_type);
	printf("check_algo = %u\n", meta->check_algo);
	printf("pfs_nmasters = %u\n", meta->pfs_nmasters);
	printf("pfs_type = %u (%s)\n", meta->pfs_type,
	    hammer2_pfstype_to_str(meta->pfs_type));
	printf("pfs_inum = 0x%jx\n", (uintmax_t)meta->pfs_inum);
	uuid = meta->pfs_clid;
	printf("pfs_clid = %s\n", hammer2_uuid_to_str(&uuid, &str));
	uuid = meta->pfs_fsid;
	printf("pfs_fsid = %s\n", hammer2_uuid_to_str(&uuid, &str));
	printf("data_quota = 0x%jx\n", (uintmax_t)meta->data_quota);
	printf("inode_quota = 0x%jx\n", (uintmax_t)meta->inode_quota);
	printf("pfs_lsnap_tid = 0x%jx\n", (uintmax_t)meta->pfs_lsnap_tid);
	printf("decrypt_check = 0x%jx\n", (uintmax_t)meta->decrypt_check);

	free(str);
}
