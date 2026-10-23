#include "hammer2.h"
#include <linux/time.h>
#include <linux/ktime.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>

int
hammer2_get_dtype(uint8_t type)
{
	switch (type) {
	case HAMMER2_OBJTYPE_UNKNOWN:
		return (DT_UNKNOWN);
	case HAMMER2_OBJTYPE_DIRECTORY:
		return (DT_DIR);
	case HAMMER2_OBJTYPE_REGFILE:
		return (DT_REG);
	case HAMMER2_OBJTYPE_FIFO:
		return (DT_FIFO);
	case HAMMER2_OBJTYPE_CDEV:
		return (DT_CHR);
	case HAMMER2_OBJTYPE_BDEV:
		return (DT_BLK);
	case HAMMER2_OBJTYPE_SOFTLINK:
		return (DT_LNK);
	case HAMMER2_OBJTYPE_SOCKET:
		return (DT_SOCK);
	case HAMMER2_OBJTYPE_WHITEOUT:
		return (DT_UNKNOWN);
	default:
		return (DT_UNKNOWN);
	}

}

int
hammer2_get_vtype(uint8_t type)
{
	switch (type) {
	case HAMMER2_OBJTYPE_UNKNOWN:
		return (VBAD);
	case HAMMER2_OBJTYPE_DIRECTORY:
		return (VDIR);
	case HAMMER2_OBJTYPE_REGFILE:
		return (VREG);
	case HAMMER2_OBJTYPE_FIFO:
		return (VFIFO);
	case HAMMER2_OBJTYPE_CDEV:
		return (VCHR);
	case HAMMER2_OBJTYPE_BDEV:
		return (VBLK);
	case HAMMER2_OBJTYPE_SOFTLINK:
		return (VLNK);
	case HAMMER2_OBJTYPE_SOCKET:
		return (VSOCK);
	case HAMMER2_OBJTYPE_WHITEOUT:
		return (VBAD);
	default:
		return (VBAD);
	}

}

uint8_t
hammer2_get_obj_type(uint8_t vtype)
{
	switch (vtype) {
	case VDIR:
		return (HAMMER2_OBJTYPE_DIRECTORY);
	case VREG:
		return (HAMMER2_OBJTYPE_REGFILE);
	case VFIFO:
		return (HAMMER2_OBJTYPE_FIFO);
	case VSOCK:
		return (HAMMER2_OBJTYPE_SOCKET);
	case VCHR:
		return (HAMMER2_OBJTYPE_CDEV);
	case VBLK:
		return (HAMMER2_OBJTYPE_BDEV);
	case VLNK:
		return (HAMMER2_OBJTYPE_SOFTLINK);
	default:
		return (HAMMER2_OBJTYPE_UNKNOWN);
	}

}

void
hammer2_time_to_timespec(uint64_t xtime, struct timespec64 *ts)
{
	ts->tv_sec = (unsigned long)(xtime / 1000000);
	ts->tv_nsec = (unsigned int)(xtime % 1000000) * 1000L;
}

uint64_t
hammer2_timespec_to_time(const struct timespec64 *ts)
{
	uint64_t xtime;

	xtime = (unsigned)(ts->tv_nsec / 1000) +
	    (unsigned long)ts->tv_sec * 1000000ULL;

	return (xtime);
}

uint32_t
hammer2_to_unix_xid(const struct uuid *uuid)
{
	return (*(const uint32_t *)&uuid->node[2]);
}

void
hammer2_guid_to_uuid(struct uuid *uuid, uint32_t guid)
{
	bzero(uuid, sizeof(*uuid));
	*(uint32_t *)&uuid->node[2] = guid;
}

hammer2_key_t
hammer2_dirhash(const char *aname, size_t len)
{
	uint32_t crcx;
	uint64_t key;
	size_t i, j;

	key = 0;

	crcx = 0;
	for (i = j = 0; i < len; ++i) {
		if (aname[i] == '.' ||
		    aname[i] == '-' ||
		    aname[i] == '_' ||
		    aname[i] == '~') {
			if (i != j)
				crcx += hammer2_icrc32(aname + j, i - j);
			j = i + 1;
		}
	}
	if (i != j)
		crcx += hammer2_icrc32(aname + j, i - j);

	crcx |= 0x80000000U;
	key |= (uint64_t)crcx << 32;

	crcx = hammer2_icrc32(aname, len);
	crcx = crcx ^ (crcx << 16);
	key |= crcx & 0xFFFF0000U;

	key |= 0x8000U;

	return (key);
}

int
hammer2_getradix(size_t bytes)
{
	int radix;

	if (bytes == HAMMER2_PBUFSIZE)
		radix = HAMMER2_PBUFRADIX;
	else if (bytes >= HAMMER2_LBUFSIZE)
		radix = HAMMER2_LBUFRADIX;
	else if (bytes >= HAMMER2_ALLOC_MIN)
		radix = HAMMER2_RADIX_MIN;
	else
		radix = 0;

	while (((size_t)1 << radix) < bytes)
		++radix;
	return (radix);
}

int
hammer2_calc_logical(hammer2_inode_t *ip, hammer2_off_t uoff,
    hammer2_key_t *lbasep, hammer2_key_t *leofp)
{
	if (lbasep)
		*lbasep = uoff & ~HAMMER2_PBUFMASK64;
	if (leofp)
		*leofp = (ip->meta.size + HAMMER2_PBUFMASK64) &
		    ~HAMMER2_PBUFMASK64;

	return (HAMMER2_PBUFSIZE);
}

int
hammer2_get_logical(void)
{
	return (hammer2_calc_logical(NULL, 0, NULL, NULL));
}

int
hammer2_calc_physical(hammer2_inode_t *ip, hammer2_key_t lbase)
{
	int lblksize, pblksize, eofbytes;

	lblksize = hammer2_calc_logical(ip, lbase, NULL, NULL);
	if (lbase + lblksize <= ip->meta.size)
		return (lblksize);
	if (lbase >= ip->meta.size)
		return (0);
	eofbytes = (int)(ip->meta.size - lbase);
	pblksize = lblksize;
	while (pblksize >= eofbytes && pblksize >= HAMMER2_ALLOC_MIN)
		pblksize >>= 1;
	pblksize <<= 1;

	return (pblksize);
}

void
hammer2_update_time(uint64_t *timep)
{
	struct timespec64 ts;

	ktime_get_real_ts64(&ts);
	*timep = (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void
hammer2_inc_iostat(hammer2_iostat_t *ios, int btype, size_t bytes)
{
	hammer2_iostat_unit_t *p = NULL;
#ifdef HAMMER2_INVARIANTS
	switch (btype) {
	case HAMMER2_BREF_TYPE_INODE:
		p = &ios->inode;
		break;
	case HAMMER2_BREF_TYPE_INDIRECT:
		p = &ios->indirect;
		break;
	case HAMMER2_BREF_TYPE_DATA:
		p = &ios->data;
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
		p = &ios->dirent;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		p = &ios->freemap_node;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		p = &ios->freemap_leaf;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP:
		p = &ios->freemap;
		break;
	case HAMMER2_BREF_TYPE_VOLUME:
		p = &ios->volume;
		break;
	default:
		KKASSERT(0);
		break;
	}
#endif
	if (p) {
		KKASSERT(bytes > 0);;
		p->count++;
		p->bytes += bytes;
	}
}

void
hammer2_print_iostat(const hammer2_iostat_t *ios, const char *msg)
{
	debug_hprintf("iostat %s count: "
	    "inode %lu indirect %lu data %lu dirent %lu freemap_node %lu "
	    "freemap_leaf %lu freemap %lu volume %lu\n",
	    msg, ios->inode.count, ios->indirect.count, ios->data.count,
	    ios->dirent.count, ios->freemap_node.count, ios->freemap_leaf.count,
	    ios->freemap.count, ios->volume.count);

	debug_hprintf("iostat %s bytes: "
	    "inode %lx indirect %lx data %lx dirent %lx freemap_node %lx "
	    "freemap_leaf %lx freemap %lx volume %lx\n",
	    msg, ios->inode.bytes, ios->indirect.bytes, ios->data.bytes,
	    ios->dirent.bytes, ios->freemap_node.bytes, ios->freemap_leaf.bytes,
	    ios->freemap.bytes, ios->volume.bytes);
}

int
hammer2_signal_check(void)
{
	if (signal_pending(current))
		return SIGINT;
	return 0;
}

const char *
hammer2_breftype_to_str(uint8_t type)
{
	switch (type) {
	case HAMMER2_BREF_TYPE_EMPTY:
		return ("empty");
	case HAMMER2_BREF_TYPE_INODE:
		return ("inode");
	case HAMMER2_BREF_TYPE_INDIRECT:
		return ("indirect");
	case HAMMER2_BREF_TYPE_DATA:
		return ("data");
	case HAMMER2_BREF_TYPE_DIRENT:
		return ("dirent");
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		return ("freemap_node");
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		return ("freemap_leaf");
	case HAMMER2_BREF_TYPE_INVALID:
		return ("invalid");
	case HAMMER2_BREF_TYPE_FREEMAP:
		return ("freemap");
	case HAMMER2_BREF_TYPE_VOLUME:
		return ("volume");
	default:
		return ("unknown");
	}
}

uid_t
hammer2_inode_to_uid(const hammer2_inode_t *ip)
{
	return (hammer2_to_unix_xid(&ip->meta.uid));
}

gid_t
hammer2_inode_to_gid(const hammer2_inode_t *ip)
{
	return (hammer2_to_unix_xid(&ip->meta.gid));
}

static int32_t
makeudev(int x, int y)
{
	if ((x & 0xffffff00) || (y & 0x0000ff00))
		return (-1);
	return ((x << 8) | y);
}

int
hammer2_getnewfsid(void *mp)
{
	(void)mp;
	(void)makeudev;
	return 0;
}
