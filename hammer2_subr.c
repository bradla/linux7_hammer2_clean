#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/uuid.h>
#include <sys/dirent.h>

#include "hammer2.h"

int
hammer2_get_dtype(uint8_t type)
{
	switch(type) {
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
	switch(type) {
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
hammer2_get_obj_type(enum vtype vtype)
{
	switch(vtype) {
	case VDIR:
		return(HAMMER2_OBJTYPE_DIRECTORY);
	case VREG:
		return(HAMMER2_OBJTYPE_REGFILE);
	case VFIFO:
		return(HAMMER2_OBJTYPE_FIFO);
	case VSOCK:
		return(HAMMER2_OBJTYPE_SOCKET);
	case VCHR:
		return(HAMMER2_OBJTYPE_CDEV);
	case VBLK:
		return(HAMMER2_OBJTYPE_BDEV);
	case VLNK:
		return(HAMMER2_OBJTYPE_SOFTLINK);
	default:
		return(HAMMER2_OBJTYPE_UNKNOWN);
	}
}

void
hammer2_time_to_timespec(uint64_t xtime, struct timespec *ts)
{
	ts->tv_sec = (unsigned long)(xtime / 1000000);
	ts->tv_nsec = (unsigned int)(xtime % 1000000) * 1000L;
}

uint64_t
hammer2_timespec_to_time(const struct timespec *ts)
{
	uint64_t xtime;

	xtime = (unsigned)(ts->tv_nsec / 1000) +
		(unsigned long)ts->tv_sec * 1000000ULL;
	return(xtime);
}

uint32_t
hammer2_to_unix_xid(const uuid_t *uuid)
{
	return(*(const uint32_t *)&uuid->node[2]);
}

void
hammer2_guid_to_uuid(uuid_t *uuid, uint32_t guid)
{
	bzero(uuid, sizeof(*uuid));
	*(uint32_t *)&uuid->node[2] = guid;
}

hammer2_key_t
hammer2_dirhash(const char *aname, size_t len)
{
	uint32_t crcx;
	uint64_t key;
	size_t i;
	size_t j;

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
	if (leofp) {
		*leofp = (ip->meta.size + HAMMER2_PBUFMASK64) &
			 ~HAMMER2_PBUFMASK64;
	}
	return (HAMMER2_PBUFSIZE);
}

int
hammer2_calc_physical(hammer2_inode_t *ip, hammer2_key_t lbase)
{
	int lblksize;
	int pblksize;
	int eofbytes;

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
	struct timespec ts;

	vfs_timestamp(&ts);
	*timep = (unsigned long)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

void
hammer2_adjreadcounter(int btype, size_t bytes)
{
	long *counterp;

	switch(btype) {
	case HAMMER2_BREF_TYPE_DATA:
		counterp = &hammer2_iod_file_read;
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
	case HAMMER2_BREF_TYPE_INODE:
		counterp = &hammer2_iod_meta_read;
		break;
	case HAMMER2_BREF_TYPE_INDIRECT:
		counterp = &hammer2_iod_indr_read;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		counterp = &hammer2_iod_fmap_read;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP:
	case HAMMER2_BREF_TYPE_VOLUME:
		counterp = &hammer2_iod_volu_read;
		break;
	case HAMMER2_BREF_TYPE_EMPTY:
	default:
		return;
	}
	*counterp += bytes;
}

void
hammer2_adjwritecounter(int btype, size_t bytes)
{
	long *counterp;

	switch(btype) {
	case HAMMER2_BREF_TYPE_DATA:
		counterp = &hammer2_iod_file_write;
		break;
	case HAMMER2_BREF_TYPE_DIRENT:
	case HAMMER2_BREF_TYPE_INODE:
		counterp = &hammer2_iod_meta_write;
		break;
	case HAMMER2_BREF_TYPE_INDIRECT:
		counterp = &hammer2_iod_indr_write;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		counterp = &hammer2_iod_fmap_write;
		break;
	case HAMMER2_BREF_TYPE_FREEMAP:
	case HAMMER2_BREF_TYPE_VOLUME:
		counterp = &hammer2_iod_volu_write;
		break;
	case HAMMER2_BREF_TYPE_EMPTY:
	default:
		return;
	}
	*counterp += bytes;
}

int
hammer2_signal_check(time_t *timep)
{
	thread_t td = curthread;
	int error = 0;

	if (td->td_lwp) {
		lwkt_user_yield();
		if (*timep != time_second) {
			*timep = time_second;
			if (CURSIG_NOBLOCK(curthread->td_lwp) != 0)
				error = HAMMER2_ERROR_ABORTED;
		}
	} else {
		lwkt_yield();
	}
	return error;
}

const char *
hammer2_error_str(int error)
{
	if (error & HAMMER2_ERROR_EIO)
		return("I/O Error");
	if (error & HAMMER2_ERROR_CHECK)
		return("Check Error");
	if (error & HAMMER2_ERROR_INCOMPLETE)
		return("Cluster Quorum Error");
	if (error & HAMMER2_ERROR_DEPTH)
		return("Chain Depth Error");
	if (error & HAMMER2_ERROR_BADBREF)
		return("Bad Blockref Error");
	if (error & HAMMER2_ERROR_ENOSPC)
		return("No Space on Device");
	if (error & HAMMER2_ERROR_ENOENT)
		return("Entry Not Found");
	if (error & HAMMER2_ERROR_ENOTEMPTY)
		return("Directory Not Empty");
	if (error & HAMMER2_ERROR_EAGAIN)
		return("EAGAIN");
	if (error & HAMMER2_ERROR_ENOTDIR)
		return("Not a Directory");
	if (error & HAMMER2_ERROR_EISDIR)
		return("Is a Directory");
	if (error & HAMMER2_ERROR_EINPROGRESS)
		return("Operation in Progress");
	if (error & HAMMER2_ERROR_ABORTED)
		return("Operation Aborted");
	if (error & HAMMER2_ERROR_EOF)
		return("Operation Complete");
	if (error & HAMMER2_ERROR_EINVAL)
		return("Invalid Operation");
	if (error & HAMMER2_ERROR_EEXIST)
		return("Object Exists");
	if (error & HAMMER2_ERROR_EDEADLK)
		return("Deadlock Detected");
	if (error & HAMMER2_ERROR_ESRCH)
		return("Object Not Found");
	if (error & HAMMER2_ERROR_ETIMEDOUT)
		return("Timeout");
	return("Unknown Error");
}

const char *
hammer2_bref_type_str(int btype)
{
	switch(btype) {
	case HAMMER2_BREF_TYPE_EMPTY:
		return("empty");
	case HAMMER2_BREF_TYPE_INODE:
		return("inode");
	case HAMMER2_BREF_TYPE_INDIRECT:
		return("indirect");
	case HAMMER2_BREF_TYPE_DATA:
		return("data");
	case HAMMER2_BREF_TYPE_DIRENT:
		return("dirent");
	case HAMMER2_BREF_TYPE_FREEMAP_NODE:
		return("freemap_node");
	case HAMMER2_BREF_TYPE_FREEMAP_LEAF:
		return("freemap_leaf");
	case HAMMER2_BREF_TYPE_INVALID:
		return("invalid");
	case HAMMER2_BREF_TYPE_FREEMAP:
		return("freemap");
	case HAMMER2_BREF_TYPE_VOLUME:
		return("volume");
	default:
		return("unknown");
	}
}
