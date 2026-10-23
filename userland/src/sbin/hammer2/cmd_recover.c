#include "hammer2.h"

#define chflags(path, flags)		(0)
#define lchflags(path, flags)		(0)
#define fchflags(fd, flags)		(0)

#define HTABLE_SIZE	(4*1024*1024)
#define HTABLE_MASK	(HTABLE_SIZE - 1)
#define DISPMODULO	(HTABLE_SIZE / 32768)

#include <openssl/sha.h>

#if 0
typedef struct dirent_entry {
	struct dirent_entry *next;
	char		*filename;
	size_t		len;
	hammer2_key_t	inum;
	hammer2_off_t	bref_off;
} dirent_entry_t;
#endif

typedef struct inode_entry {
	struct inode_entry *next;
	struct inode_entry *next2;
	hammer2_off_t	data_off;
	hammer2_key_t	inum;

	char		*link_file_path;
	uint32_t	inode_crc;
	uint8_t		type;
	uint8_t		encountered;
	uint8_t		loopcheck;
	uint8_t		unused03;
} inode_entry_t;

typedef struct topology_entry {
	struct topology_entry *next;
	char		*path;
	long		iterator;
} topology_entry_t;

typedef struct neg_entry {
	struct neg_entry *next;
	hammer2_blockref_t bref;
} neg_entry_t;

typedef struct topo_bref_entry {
	struct topo_bref_entry *next;
	topology_entry_t  *topo;
	hammer2_off_t	data_off;
} topo_bref_entry_t;

typedef struct topo_inode_entry {
	struct topo_inode_entry *next;
	topology_entry_t  *topo;
	inode_entry_t *iscan;
} topo_inode_entry_t;

static inode_entry_t **InodeHash;
static inode_entry_t **InodeHash2;
static topology_entry_t **TopologyHash;
static neg_entry_t **NegativeHash;
static topo_bref_entry_t **TopoBRefHash;
static topo_inode_entry_t **TopoInodeHash;

static int check_filename(hammer2_blockref_t *bref,
			const char *filename, char *buf, size_t flen);

static topology_entry_t *enter_topology(const char *path);
static int topology_check_duplicate_inode(topology_entry_t *topo,
			inode_entry_t *iscan);
static int topology_check_duplicate_indirect(topology_entry_t *topo,
			hammer2_blockref_t *bref);
static void enter_inode(hammer2_blockref_t *bref);
static void enter_inode_untested(hammer2_inode_data_t *ip, hammer2_off_t loff);
static inode_entry_t *find_first_inode(hammer2_key_t inum);

static int find_neg(hammer2_blockref_t *bref);
static void enter_neg(hammer2_blockref_t *bref);
static void dump_tree(inode_entry_t *iscan, const char *dest,
			const char *remain, int depth, int path_depth,
			int isafile);
static int dump_inum_file(inode_entry_t *iscan, hammer2_inode_data_t *inode,
			const char *path);
static int dump_inum_softlink(hammer2_inode_data_t *inode, const char *path);
static int dump_dir_data(const char *dest, const char *remain,
			hammer2_blockref_t *base, int count,
			int depth, int path_depth, int isafile);
static int dump_file_data(int wfd, hammer2_off_t fsize,
			hammer2_blockref_t *bref, int count);
static int validate_crc(hammer2_blockref_t *bref, void *data, size_t bytes);
static uint32_t hammer2_to_unix_xid(const hammer2_uuid_t *uuid);
static void *hammer2_cache_read(hammer2_off_t data_off, size_t *bytesp);

static long InodeCount;
static long TopoBRefCount;
static long TopoBRefDupCount;
static long TopoInodeCount;
static long TopoInodeDupCount;
static long NegativeCount;
static long NegativeHits;
static long MediaBytes;
static int StrictMode = 1;

#define INODES_PER_BLOCK	\
	(sizeof(union hammer2_media_data) / sizeof(hammer2_inode_data_t))
#define REPINODEDEPTH		256

int
cmd_recover(const char *devpath, const char *pathname,
	    const char *destdir, int strict, int isafile)
{
	hammer2_media_data_t data;
	hammer2_volume_t *vol;
	hammer2_off_t loff;
	hammer2_off_t poff;
	struct stat st;
	size_t i;

	if (stat(destdir, &st) == -1) {
		perror("stat");
		return 1;
	} else if (!S_ISDIR(st.st_mode)) {
		fprintf(stderr, "%s: not a directory\n", destdir);
		return 1;
	}

	StrictMode = strict;
	hammer2_init_volumes(devpath, 1);

	InodeHash = calloc(HTABLE_SIZE, sizeof(inode_entry_t *));
	InodeHash2 = calloc(HTABLE_SIZE, sizeof(inode_entry_t *));
	TopologyHash = calloc(HTABLE_SIZE, sizeof(topology_entry_t *));
	NegativeHash = calloc(HTABLE_SIZE, sizeof(neg_entry_t *));
	TopoBRefHash = calloc(HTABLE_SIZE, sizeof(topo_bref_entry_t *));
	TopoInodeHash = calloc(HTABLE_SIZE, sizeof(topo_inode_entry_t *));

	printf("MEDIA PASS\n");

	loff = 0;
	while ((vol = hammer2_get_volume(loff)) != NULL) {
		int fd;
		int xdisp;

		fd = vol->fd;
		poff = loff - vol->offset;
		xdisp = 0;

		while (poff < vol->size) {
			if (pread(fd, &data, sizeof(data), poff) !=
			    sizeof(data))
			{

				poff += sizeof(data);
				continue;
			}
			for (i = 0; i < HAMMER2_IND_COUNT_MAX; ++i) {
				hammer2_blockref_t *bref;

				bref = &data.npdata[i];

				switch(bref->type) {
				case HAMMER2_BREF_TYPE_INODE:

					enter_inode(bref);
					break;
				case HAMMER2_BREF_TYPE_DIRENT:

#if 0
					if (filename &&
					    flen != bref->embed.dirent.namlen)
					{

						break;
					}
					if (check_filename(bref, filename,
						   filename_buf,
						   bref->embed.dirent.namlen))
					{
						enter_dirent(bref,
						    poff + vol->offset +
						    (i *
						    sizeof(hammer2_blockref_t)),
						    filename_buf,
						    bref->embed.dirent.namlen);
					}
#endif
					break;
				default:
					break;
				}
			}

			for (i = 0; i < INODES_PER_BLOCK; ++i) {
				hammer2_inode_data_t *ip;

				ip = (void *)(data.buf + i * sizeof(*ip));
				if (ip->meta.inum == 1 &&
				    ip->meta.iparent == 0 &&
				    ip->meta.type ==
					HAMMER2_OBJTYPE_DIRECTORY &&
				    ip->meta.op_flags & HAMMER2_OPFLAG_PFSROOT)
				{
					enter_inode_untested(ip,
						poff + vol->offset +
						(i * sizeof(*ip)));
				}
			}
			poff += sizeof(data);

			MediaBytes += sizeof(data);

			if (QuietOpt == 0 &&
			    (++xdisp == DISPMODULO ||
			     poff == vol->size - sizeof(data)))
			{
				xdisp = 0;
				printf("%ld inodes scanned, "
				       "media %6.2f/%-3.2fG\r",
					InodeCount,
					MediaBytes / 1e9,
					hammer2_get_total_size() / 1e9);
				fflush(stdout);
			}
		}
		loff = vol->offset + vol->size;
	}

	printf("\nInodes=%ld, Invalid_brefs=%ld, Invalid_hits=%ld\n",
		InodeCount, NegativeCount, NegativeHits);
	printf("RESTORATION PASS\n");

	{
		int abspath = 0;
		long root_count = 0;
		long root_max = 0;

		if (pathname[0] == '/') {
			abspath = 1;
			while (*pathname == '/')
				++pathname;
		}

		{
			inode_entry_t *iscan;

			for (iscan = InodeHash[1];
			     iscan;
			     iscan = iscan->next)
			{
				if (iscan->inum == 1)
					++root_max;
			}
		}

		for (i = 0; i < HTABLE_SIZE; ++i) {
			inode_entry_t *iscan;

			for (iscan = InodeHash[i];
			     iscan;
			     iscan = iscan->next)
			{

				if (abspath && iscan->inum != 1)
					continue;
				if (iscan->type != HAMMER2_OBJTYPE_DIRECTORY)
					continue;

				if (i == 1 && iscan->inum == 1 &&
				    QuietOpt == 0)
				{
					printf("scan roots %p 0x%016jx "
					       "(count %ld/%ld)\r",
						iscan,
						iscan->data_off,
						++root_count, root_max);
					fflush(stdout);
				}

				dump_tree(iscan, destdir, pathname,
					  1, 1, isafile);
			}
			if (QuietOpt == 0 &&
			    (i & (DISPMODULO - 1)) == DISPMODULO - 1)
			{
				if (i == DISPMODULO - 1)
					printf("\n");
				printf("Progress %zd/%d\r", i, HTABLE_SIZE);
				fflush(stdout);
			}
		}
		printf("\n");
	}

	printf("CLEANUP\n");
	printf("TopoBRef stats: count=%ld dups=%ld\n",
	       TopoBRefCount, TopoBRefDupCount);
	printf("TopoInode stats: count=%ld dups=%ld\n",
	       TopoInodeCount, TopoInodeDupCount);

	hammer2_cleanup_volumes();

	for (i = 0; i < HTABLE_SIZE; ++i) {

		inode_entry_t *iscan;
		topology_entry_t *top_scan;
		neg_entry_t *negscan;
		topo_bref_entry_t *topo_bref;
		topo_inode_entry_t *topo_inode;

		while ((iscan = InodeHash[i]) != NULL) {
			InodeHash[i] = iscan->next;
			free(iscan);
		}

		while ((top_scan = TopologyHash[i]) != NULL) {
			TopologyHash[i] = top_scan->next;
			free(top_scan->path);
			free(top_scan);
		}

		while ((negscan = NegativeHash[i]) != NULL) {
			NegativeHash[i] = negscan->next;
			free(negscan);
		}

		while ((topo_bref = TopoBRefHash[i]) != NULL) {
			TopoBRefHash[i] = topo_bref->next;
			free(topo_bref);
		}

		while ((topo_inode = TopoInodeHash[i]) != NULL) {
			TopoInodeHash[i] = topo_inode->next;
			free(topo_inode);
		}
	}
	free(TopoInodeHash);
	free(TopoBRefHash);
	free(NegativeHash);
	free(TopologyHash);

	free(InodeHash);
	free(InodeHash2);

	return 0;
}

static int
check_filename(hammer2_blockref_t *bref, const char *filename, char *buf,
	       size_t flen)
{

	if (flen > 1024)
		return 0;

	if (flen <= 64) {

		if (buf)
			bcopy(bref->check.buf, buf, flen);
		buf[flen] = 0;
		if (filename == NULL)
			return 1;
		if (bcmp(filename, bref->check.buf, flen) == 0)
			return 1;
	} else {

		hammer2_media_data_t data;
		hammer2_volume_t *vol;
		hammer2_off_t poff;
		hammer2_off_t psize;
		int vfd;

		if ((bref->data_off & 0x1F) == 0 ||
		    (bref->data_off & 0x1F) > 10)
		{
			return 0;
		}

		psize = 1 << (bref->data_off & 0x1F);
		if (flen > psize)
			return 0;

		if (StrictMode) {
			if (HAMMER2_DEC_CHECK(bref->methods) ==
				HAMMER2_CHECK_NONE ||
			    HAMMER2_DEC_CHECK(bref->methods) ==
				HAMMER2_CHECK_DISABLED)
			{
				return 0;
			}
		}

		vol = hammer2_get_volume(bref->data_off);
		if (vol == NULL)
			return 0;

		vfd = vol->fd;
		poff = (bref->data_off - vol->offset) & ~0x1FL;
		if (pread(vfd, &data, psize, poff) != (ssize_t)psize)
			return 0;

		if (validate_crc(bref, &data, psize) == 0)
			return 0;

		if (buf)
			bcopy(data.buf, buf, flen);
		buf[flen] = 0;
		if (filename == NULL)
			return 1;
		if (bcmp(filename, data.buf, flen) == 0)
			return 1;
	}
	return 0;
}

#if 0
static void
enter_dirent(hammer2_blockref_t *bref, hammer2_off_t loff,
	     const char *filename, size_t flen)
{
	hammer2_key_t inum = bref->embed.dirent.inum;
	dirent_entry_t *entry;
	uint32_t hv = (inum ^ (inum >> 16)) & HTABLE_MASK;

	for (entry = DirHash[hv]; entry; entry = entry->next) {
		if (entry->inum == inum &&
		    entry->len == flen &&
		    bcmp(entry->filename, filename, flen) == 0)
		{
			return;
		}
	}
	entry = malloc(sizeof(*entry));
	bzero(entry, sizeof(*entry));
	entry->inum = inum;
	entry->next = DirHash[hv];
	entry->filename = malloc(flen + 1);
	entry->len = flen;
	entry->bref_off = loff;
	bcopy(filename, entry->filename, flen);
	entry->filename[flen] = 0;
	DirHash[hv] = entry;
}
#endif

static topology_entry_t *
enter_topology(const char *path)
{
	topology_entry_t *topo;
	uint32_t hv = 0;
	size_t i;

	for (i = 0; path[i]; ++i)
		hv = (hv << 5) ^ path[i] ^ (hv >> 24);
	hv = (hv ^ (hv >> 16)) & HTABLE_MASK;
	for (topo = TopologyHash[hv]; topo; topo = topo->next) {
		if (strcmp(path, topo->path) == 0)
			return topo;
	}
	topo = malloc(sizeof(*topo));
	bzero(topo, sizeof(*topo));

	topo->next = TopologyHash[hv];
	TopologyHash[hv] = topo;
	topo->path = strdup(path);
	topo->iterator = 1;

	return topo;
}

static int
topology_check_duplicate_inode(topology_entry_t *topo, inode_entry_t *iscan)
{
	int hv = (((intptr_t)topo ^ (intptr_t)iscan) >> 6) & HTABLE_MASK;
	topo_inode_entry_t *scan;

	for (scan = TopoInodeHash[hv]; scan; scan = scan->next) {
		if (scan->topo == topo &&
		    scan->iscan == iscan)
		{
			++TopoInodeDupCount;
			return 1;
		}
	}
	scan = malloc(sizeof(*scan));
	bzero(scan, sizeof(*scan));
	scan->iscan = iscan;
	scan->topo = topo;
	scan->next = TopoInodeHash[hv];
	TopoInodeHash[hv] = scan;
	++TopoInodeCount;

	return 0;
}

static int
topology_check_duplicate_indirect(topology_entry_t *topo,
				  hammer2_blockref_t *bref)
{
	int hv = ((intptr_t)topo ^ (bref->data_off >> 8)) & HTABLE_MASK;
	topo_bref_entry_t *scan;

	for (scan = TopoBRefHash[hv]; scan; scan = scan->next) {
		if (scan->topo == topo &&
		    scan->data_off == bref->data_off)
		{
			++TopoBRefDupCount;
			return 1;
		}
	}
	scan = malloc(sizeof(*scan));
	bzero(scan, sizeof(*scan));
	scan->data_off = bref->data_off;
	scan->topo = topo;
	scan->next = TopoBRefHash[hv];
	TopoBRefHash[hv] = scan;
	++TopoBRefCount;

	return 0;
}

static void
enter_inode(hammer2_blockref_t *bref)
{
	uint32_t hv;
	uint32_t hv2;
	inode_entry_t *scan;
	hammer2_inode_data_t *inode;
	size_t psize;

	hv = (bref->key ^ (bref->key >> 16)) & HTABLE_MASK;
	hv2 = (bref->key ^ (bref->key >> 16) ^ (bref->data_off >> 10)) &
	      HTABLE_MASK;

	for (scan = InodeHash2[hv2]; scan; scan = scan->next2) {
		if (bref->key == scan->inum &&
		    bref->data_off == scan->data_off)
		{
			return;
		}
	}

	if (find_neg(bref))
		return;

	if ((1 << (bref->data_off & 0x1F)) != sizeof(*inode))
		return;
	if ((bref->data_off & ~0x1FL & (sizeof(*inode) - 1)) != 0)
		return;
	if (bref->keybits != 0)
		return;
	if (bref->key == 0)
		return;

	inode = hammer2_cache_read(bref->data_off, &psize);

	if (psize == 0)
		return;

	if (inode == NULL) {
fail:
		enter_neg(bref);
		return;
	}

	if (validate_crc(bref, inode, sizeof(*inode)) == 0)
		goto fail;
	if (inode->meta.inum != bref->key)
		goto fail;

	scan = malloc(sizeof(*scan));
	bzero(scan, sizeof(*scan));

	scan->inum = bref->key;
	scan->type = inode->meta.type;
	scan->data_off = bref->data_off;
	scan->inode_crc = hammer2_icrc32(inode, sizeof(*inode));

	scan->next = InodeHash[hv];
	InodeHash[hv] = scan;
	scan->next2 = InodeHash2[hv2];
	InodeHash2[hv2] = scan;

	++InodeCount;
}

static void
enter_inode_untested(hammer2_inode_data_t *ip, hammer2_off_t loff)
{
	uint32_t hv;
	uint32_t hv2;
	inode_entry_t *scan;
	int radix;

	for (radix = 0; ((size_t)1 << radix) < sizeof(*ip); ++radix)
		;
	loff |= radix;

	hv = (ip->meta.inum ^ (ip->meta.inum >> 16)) & HTABLE_MASK;
	hv2 = (ip->meta.inum ^ (ip->meta.inum >> 16) ^ (loff >> 10)) &
	      HTABLE_MASK;

	for (scan = InodeHash2[hv2]; scan; scan = scan->next2) {
		if (ip->meta.inum == scan->inum &&
		    loff == scan->data_off)
		{
			return;
		}
	}

	scan = malloc(sizeof(*scan));
	bzero(scan, sizeof(*scan));

	scan->inum = ip->meta.inum;
	scan->type = ip->meta.type;
	scan->data_off = loff;
	scan->inode_crc = hammer2_icrc32(ip, sizeof(*ip));

	scan->next = InodeHash[hv];
	InodeHash[hv] = scan;
	scan->next2 = InodeHash2[hv2];
	InodeHash2[hv2] = scan;

	++InodeCount;
}

static inode_entry_t *
find_first_inode(hammer2_key_t inum)
{
	inode_entry_t *entry;
	uint32_t hv = (inum ^ (inum >> 16)) & HTABLE_MASK;

	for (entry = InodeHash[hv]; entry; entry = entry->next) {
		if (entry->inum == inum)
			return entry;
	}
	return NULL;
}

static int
find_neg(hammer2_blockref_t *bref)
{
	int hv = (bref->data_off >> 10) & HTABLE_MASK;
	neg_entry_t *neg;

	for (neg = NegativeHash[hv]; neg; neg = neg->next) {
		if (bref->data_off == neg->bref.data_off &&
		    bref->type == neg->bref.type &&
		    bref->methods == neg->bref.methods &&
		    bref->key == neg->bref.key &&
		    bcmp(&bref->check, &neg->bref.check,
			 sizeof(bref->check)) == 0)
		{
			++NegativeHits;
			return 1;
		}
	}
	return 0;
}

static void
enter_neg(hammer2_blockref_t *bref)
{
	int hv = (bref->data_off >> 10) & HTABLE_MASK;
	neg_entry_t *neg;

	neg = malloc(sizeof(*neg));
	bzero(neg, sizeof(*neg));
	neg->next = NegativeHash[hv];
	neg->bref = *bref;
	NegativeHash[hv] = neg;
	++NegativeCount;
}

static void
dump_tree(inode_entry_t *iscan, const char *dest, const char *remain,
	  int depth, int path_depth, int isafile)
{
	topology_entry_t *topo;
	hammer2_inode_data_t *inode;
	hammer2_inode_data_t inode_copy;
	struct stat st;
	size_t psize;
	char *path;

	inode = hammer2_cache_read(iscan->data_off, &psize);
	if (psize == 0)
		return;
	if (inode == NULL || psize != sizeof(*inode))
		return;
	if (iscan->inode_crc != hammer2_icrc32(inode, sizeof(*inode)))
		return;
	inode_copy = *inode;
	inode = &inode_copy;

	if (depth > REPINODEDEPTH && iscan->encountered)
		return;

	while (*remain == '/')
		++remain;

	topo = enter_topology(dest);
	if (topology_check_duplicate_inode(topo, iscan))
		return;

	switch(iscan->type) {
	case HAMMER2_OBJTYPE_DIRECTORY:

		if (isafile && *remain == 0)
			return;

		if (depth != 1) {
			if (mkdir(dest, 0755) == 0)
				iscan->encountered = 1;
			if (stat(dest, &st) == 0) {
				if (0)
					chflags(dest, 0);
				if ((st.st_mode & 0700) != 0700)
					chmod(dest, 0755);
			}
		}

		(void)dump_dir_data(dest, remain,
				    &inode->u.blockset.blockref[0],
				    HAMMER2_SET_COUNT, depth, path_depth + 1,
				    isafile);

		if (depth != 1) {
			struct timeval tvs[2];

			tvs[0].tv_sec = inode->meta.atime / 1000000;
			tvs[0].tv_usec = inode->meta.atime % 1000000;
			tvs[1].tv_sec = inode->meta.mtime / 1000000;
			tvs[1].tv_usec = inode->meta.mtime % 1000000;

			if (lutimes(dest, tvs) < 0)
				perror("futimes");
			lchown(dest,
			       hammer2_to_unix_xid(&inode->meta.uid),
			       hammer2_to_unix_xid(&inode->meta.gid));

			lchmod(dest, inode->meta.mode);
			lchflags(dest, inode->meta.uflags);
		}
		break;
	case HAMMER2_OBJTYPE_REGFILE:

		if (*remain == 0) {
			asprintf(&path, "%s.%05ld", dest, topo->iterator);
			++topo->iterator;

			if (stat(path, &st) == 0) {
				if (0)
					chflags(path, 0);
				if ((st.st_mode & 0600) != 0600)
					chmod(path, 0644);
			}
			iscan->encountered = 1;
			(void)dump_inum_file(iscan, inode, path);
			free(path);
		}
		break;
	case HAMMER2_OBJTYPE_SOFTLINK:

		if (*remain == 0) {
			asprintf(&path, "%s.%05ld", dest, topo->iterator);
			++topo->iterator;

			if (stat(path, &st) == 0) {
				if (0)
					chflags(path, 0);
				if ((st.st_mode & 0600) != 0600)
					chmod(path, 0644);
			}
			(void)dump_inum_softlink(inode, path);
			free(path);
		}
		break;
	}
}

static int
dump_inum_file(inode_entry_t *iscan, hammer2_inode_data_t *inode,
	       const char *path1)
{
	char *path2;
	int wfd;
	int res;

	if (iscan->link_file_path) {
		if (link(iscan->link_file_path, path1) == 0)
			return 1;
		chflags(iscan->link_file_path, 0);
		chmod(iscan->link_file_path, 0600);
		if (link(iscan->link_file_path, path1) == 0) {
			chmod(iscan->link_file_path, inode->meta.mode);
			chflags(iscan->link_file_path, inode->meta.uflags);
			return 1;
		}
	}

	chflags(path1, 0);
	chmod(path1, 0600);
	wfd = open(path1, O_RDWR|O_CREAT|O_TRUNC, 0600);

	if (inode->meta.op_flags & HAMMER2_OPFLAG_DIRECTDATA) {

		if (inode->meta.size > 0 &&
		    inode->meta.size <= sizeof(inode->u.data))
		{
			write(wfd, inode->u.data, inode->meta.size);
		}
		res = 1;
	} else {

		res = dump_file_data(wfd, inode->meta.size,
				     &inode->u.blockset.blockref[0],
				     HAMMER2_SET_COUNT);
	}

	if (res) {
		struct timeval tvs[2];

		tvs[0].tv_sec = inode->meta.atime / 1000000;
		tvs[0].tv_usec = inode->meta.atime % 1000000;
		tvs[1].tv_sec = inode->meta.mtime / 1000000;
		tvs[1].tv_usec = inode->meta.mtime % 1000000;

		ftruncate(wfd, inode->meta.size);
		if (futimes(wfd, tvs) < 0)
			perror("futimes");
		fchown(wfd,
		       hammer2_to_unix_xid(&inode->meta.uid),
		       hammer2_to_unix_xid(&inode->meta.gid));

		fchmod(wfd, inode->meta.mode);
		fchflags(wfd, inode->meta.uflags);
		iscan->link_file_path = strdup(path1);
	} else {
		struct timeval tvs[2];

		tvs[0].tv_sec = inode->meta.atime / 1000000;
		tvs[0].tv_usec = inode->meta.atime % 1000000;
		tvs[1].tv_sec = inode->meta.mtime / 1000000;
		tvs[1].tv_usec = inode->meta.mtime % 1000000;

		ftruncate(wfd, inode->meta.size);
		if (futimes(wfd, tvs) < 0)
			perror("futimes");
		fchown(wfd,
		       hammer2_to_unix_xid(&inode->meta.uid),
		       hammer2_to_unix_xid(&inode->meta.gid));

		asprintf(&path2, "%s.corrupted", path1);
		rename(path1, path2);
		iscan->link_file_path = path2;
	}

	close(wfd);

	return res;
}

static int
dump_inum_softlink(hammer2_inode_data_t *inode __unused,
		   const char *path __unused)
{
	return 0;
}

static int
dump_dir_data(const char *dest, const char *remain,
	      hammer2_blockref_t *base, int count,
	      int depth, int path_depth, int isafile)
{
	hammer2_media_data_t data;
	hammer2_volume_t *vol;
	hammer2_off_t poff;
	size_t psize;
	int res = 1;
	int rtmp;
	int n;
	size_t flen;

	for (flen = 0; remain[flen] && remain[flen] != '/'; ++flen)
		;

	for (n = 0; n < count; ++n) {
		hammer2_blockref_t *bref;
		char filename_buf[1024+1];
		topology_entry_t *topo;

		bref = &base[n];

		if (bref->type == HAMMER2_BREF_TYPE_EMPTY)
			continue;

		vol = NULL;
		poff = 0;
		psize = 0;
		if (bref->data_off & 0x1F) {
			vol = hammer2_get_volume(bref->data_off);
			if (vol == NULL) {
				res = 0;
				continue;
			}
			poff = (bref->data_off - vol->offset) & ~0x1FL;
			psize = 1 << (bref->data_off & 0x1F);
			if (psize > sizeof(data)) {
				res = 0;
				continue;
			}
		}

		switch(bref->type) {
		case HAMMER2_BREF_TYPE_DIRENT:

			if ((flen == 0 &&
			    check_filename(bref, NULL, filename_buf,
					   bref->embed.dirent.namlen)) ||
			    (flen &&
			     flen == bref->embed.dirent.namlen &&
			     check_filename(bref, remain, filename_buf, flen)))
			{
				inode_entry_t *iscan;
				hammer2_key_t inum;
				char *path;

				inum = bref->embed.dirent.inum;

				asprintf(&path, "%s/%s", dest, filename_buf);
				iscan = find_first_inode(inum);
				while (iscan) {
					if (iscan->inum == inum &&
					    iscan->loopcheck == 0 &&
					    iscan->type ==
					     bref->embed.dirent.type)
					{
						iscan->loopcheck = 1;
						dump_tree(iscan, path,
							  remain + flen,
							  depth + 1,
							  path_depth,
							  isafile);

						iscan->loopcheck = 0;
					}
					iscan = iscan->next;
				}
				free(path);
			}
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
			if (psize == 0) {
				res = 0;
				break;
			}

			topo = enter_topology(dest);
			if (topology_check_duplicate_indirect(topo, bref))
				break;

			if (pread(vol->fd, &data, psize, poff) !=
			    (ssize_t)psize)
			{
				res = 0;
				break;
			}

			if (validate_crc(bref, &data, psize) == 0) {
				res = 0;
				break;
			}

			rtmp = dump_dir_data(dest, remain,
					  &data.npdata[0],
					  psize / sizeof(hammer2_blockref_t),
					  depth, path_depth, isafile);
			if (res)
				res = rtmp;
			break;
		}
	}
	return res;
}

static int
dump_file_data(int wfd, hammer2_off_t fsize,
	       hammer2_blockref_t *base, int count)
{
	hammer2_media_data_t data;
	hammer2_blockref_t *bref;
	hammer2_volume_t *vol;
	hammer2_off_t poff;
	hammer2_off_t psize;
	hammer2_off_t nsize;
	int res = 1;
	int rtmp;
	int n;

	for (n = 0; n < count; ++n) {
		char *dptr;
		int res2;

		bref = &base[n];

		if (bref->type == HAMMER2_BREF_TYPE_EMPTY ||
		    bref->data_off == 0)
		{
			continue;
		}
		vol = hammer2_get_volume(bref->data_off);
		if (vol == NULL)
			continue;

		poff = (bref->data_off - vol->offset) & ~0x1FL;
		psize = 1 << (bref->data_off & 0x1F);
		if (psize > sizeof(data)) {
			res = 0;
			continue;
		}

		if (pread(vol->fd, &data, psize, poff) != (ssize_t)psize) {
			res = 0;
			continue;
		}

		if (validate_crc(bref, &data, psize) == 0) {
			res = 0;
			continue;
		}

		switch(bref->type) {
		case HAMMER2_BREF_TYPE_DATA:
			dptr = (void *)&data;

			nsize = 1L << bref->keybits;
			if (nsize > sizeof(data)) {
				res = 0;
				break;
			}

			switch (HAMMER2_DEC_COMP(bref->methods)) {
			case HAMMER2_COMP_LZ4:
				dptr = hammer2_decompress_LZ4(dptr, psize,
							      nsize, &res2);
				if (res)
					res = res2;
				psize = nsize;
				break;
			case HAMMER2_COMP_ZLIB:
				dptr = hammer2_decompress_ZLIB(dptr, psize,
							       nsize, &res2);
				if (res)
					res = res2;
				psize = nsize;
				break;
			case HAMMER2_COMP_NONE:
			default:

				break;
			}

			if (bref->key + psize > fsize)
				pwrite(wfd, dptr, fsize - bref->key,
				       bref->key);
			else
				pwrite(wfd, dptr, psize, bref->key);
			break;
		case HAMMER2_BREF_TYPE_INDIRECT:
			rtmp = dump_file_data(wfd, fsize,
					  &data.npdata[0],
					  psize / sizeof(hammer2_blockref_t));
			if (res)
				res = rtmp;
			break;
		}
	}
	return res;
}

static int
validate_crc(hammer2_blockref_t *bref, void *data, size_t bytes)
{
	uint32_t cv;
	uint64_t cv64;
	SHA256_CTX hash_ctx;
	int success = 0;
	union {
		uint8_t digest[SHA256_DIGEST_LENGTH];
		uint64_t digest64[SHA256_DIGEST_LENGTH/8];
	} u;

	switch(HAMMER2_DEC_CHECK(bref->methods)) {
	case HAMMER2_CHECK_NONE:
		if (StrictMode == 0)
			success = 1;
		break;
	case HAMMER2_CHECK_DISABLED:
		if (StrictMode == 0)
			success = 1;
		break;
	case HAMMER2_CHECK_ISCSI32:
		cv = hammer2_icrc32(data, bytes);
		if (bref->check.iscsi32.value == cv) {
			success = 1;
		}
		break;
	case HAMMER2_CHECK_XXHASH64:
		cv64 = XXH64(data, bytes, XXH_HAMMER2_SEED);
		if (bref->check.xxhash64.value == cv64) {
			success = 1;
		}
		break;
	case HAMMER2_CHECK_SHA192:
		SHA256_Init(&hash_ctx);
		SHA256_Update(&hash_ctx, data, bytes);
		SHA256_Final(u.digest, &hash_ctx);
		u.digest64[2] ^= u.digest64[3];
		if (memcmp(u.digest, bref->check.sha192.data,
			   sizeof(bref->check.sha192.data)) == 0)
		{
			success = 1;
		}
		break;
	case HAMMER2_CHECK_FREEMAP:
		cv = hammer2_icrc32(data, bytes);
		if (bref->check.freemap.icrc32 == cv) {
			success = 1;
		}
		break;
	}
	return success;
}

static uint32_t
hammer2_to_unix_xid(const hammer2_uuid_t *uuid)
{

        return(*(const uint32_t *)((const uint8_t *)uuid + 12));
}

#define SDCCOUNT	16
#define SDCMASK		(SDCCOUNT - 1)

typedef struct sdccache {
	char		buf[HAMMER2_PBUFSIZE];
	hammer2_off_t	offset;
	hammer2_volume_t *vol;
	int64_t		last;
} sdccache_t;

static sdccache_t SDCCache[SDCCOUNT];
static int64_t SDCLast;

static void *
hammer2_cache_read(hammer2_off_t data_off, size_t *bytesp)
{
	hammer2_off_t poff;
	hammer2_off_t pbase;
	hammer2_volume_t *vol;
	sdccache_t *sdc;
	sdccache_t *sdc_worst;
	int i;

	*bytesp = 0;

	vol = hammer2_get_volume(data_off);
        if (vol == NULL)
                return NULL;
        poff = (data_off - vol->offset) & ~0x1FL;
	*bytesp = 1 << (data_off & 0x1F);

	pbase = poff & ~HAMMER2_PBUFMASK64;

	if ((poff ^ (poff + *bytesp - 1)) & ~HAMMER2_PBUFMASK64) {
		*bytesp = 0;
		return NULL;
	}

	if (poff & ~0x1FL & (*bytesp - 1)) {
		*bytesp = 0;
		return NULL;
	}

	sdc_worst = NULL;
	for (i = 0; i < SDCCOUNT; ++i) {
		sdc = &SDCCache[i];

		if (sdc->vol == vol && sdc->offset == pbase) {
			sdc->last = ++SDCLast;
			return (&sdc->buf[poff - pbase]);
		}
		if (sdc_worst == NULL || sdc_worst->last > sdc->last)
			sdc_worst = sdc;
	}

	sdc = sdc_worst;
	sdc->vol = vol;
	sdc->offset = pbase;
	sdc->last = ++SDCLast;

	if (pread(vol->fd, sdc->buf, HAMMER2_PBUFSIZE, pbase) !=
	    HAMMER2_PBUFSIZE)
	{
		sdc->offset = (hammer2_off_t)-1;
		sdc->last = 0;
		return NULL;
	}
	return (&sdc->buf[poff - pbase]);
}
