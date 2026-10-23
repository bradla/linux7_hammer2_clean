#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <assert.h>
#include <err.h>
#include <uuid/uuid.h>

#include <vfs/hammer2/hammer2_disk.h>
#include <vfs/hammer2/hammer2_xxhash.h>

#include "mkfs_hammer2.h"
#include "hammer2_subs.h"

#include "../lib/libc/gen/util.h"
#include "../sys/libkern/util.h"

static uint64_t nowtime(void);
static int blkrefary_cmp(const void *b1, const void *b2);
static void alloc_direct(hammer2_off_t *basep, hammer2_blockref_t *bref,
				size_t bytes);

static int
get_hammer2_version(void)
{
	int version = HAMMER2_VOL_VERSION_DEFAULT;
	size_t olen = sizeof(version);

	if (sysctlbyname("vfs.hammer2.supported_version",
			 &version, &olen, NULL, 0) == 0) {
		if (version >= HAMMER2_VOL_VERSION_WIP) {
			version = HAMMER2_VOL_VERSION_WIP - 1;
			fprintf(stderr,
				"newfs_hammer2: WARNING: HAMMER2 VFS "
				"supports higher version than I "
				"understand.\n"
				"Using default version %d\n",
				version);
		}
	} else {
		fprintf(stderr,
			"newfs_hammer2: WARNING: HAMMER2 VFS not "
			"loaded, cannot get version info.\n"
			"Using default version %d\n",
			version);
	}
	return(version);
}

void
hammer2_mkfs_init(hammer2_mkfs_options_t *opt)
{
	memset(opt, 0, sizeof(*opt));

	opt->Hammer2Version = get_hammer2_version();
	opt->Label[opt->NLabels++] = strdup("LOCAL");
	opt->CompType = HAMMER2_COMP_NEWFS_DEFAULT;
	opt->CheckType = HAMMER2_CHECK_XXHASH64;
	opt->DefaultLabelType = HAMMER2_LABEL_NONE;

	srandom(time(NULL));
	hammer2_uuid_create(&opt->Hammer2_VolFSID);
	hammer2_uuid_create(&opt->Hammer2_SupCLID);
	hammer2_uuid_create(&opt->Hammer2_SupFSID);

	if (hammer2_uuid_from_string(HAMMER2_UUID_STRING, &opt->Hammer2_FSType)) {
		errx(1, "uuids file does not have the DragonFly "
			"HAMMER2 filesystem type");
	}
}

void
hammer2_mkfs_cleanup(hammer2_mkfs_options_t *opt)
{
	int i;

	for (i = 0; i < opt->NLabels; i++)
		free(opt->Label[i]);
}

static void
adjust_options(hammer2_ondisk_t *fso, hammer2_mkfs_options_t *opt)
{

	switch (opt->DefaultLabelType) {
	case HAMMER2_LABEL_BOOT:
		opt->Label[opt->NLabels++] = strdup("BOOT");
		break;
	case HAMMER2_LABEL_ROOT:
		opt->Label[opt->NLabels++] = strdup("ROOT");
		break;
	case HAMMER2_LABEL_DATA:
		opt->Label[opt->NLabels++] = strdup("DATA");
		break;
	case HAMMER2_LABEL_NONE:

		break;
	default:
		assert(0);
		break;
	}

	hammer2_off_t BootAreaSize = opt->BootAreaSize;
	if (BootAreaSize == 0) {
		BootAreaSize = HAMMER2_BOOT_NOM_BYTES;
		while (BootAreaSize > fso->total_size / 20)
			BootAreaSize >>= 1;
		if (BootAreaSize < HAMMER2_BOOT_MIN_BYTES)
			BootAreaSize = HAMMER2_BOOT_MIN_BYTES;
	} else if (BootAreaSize < HAMMER2_BOOT_MIN_BYTES) {
		BootAreaSize = HAMMER2_BOOT_MIN_BYTES;
	}
	BootAreaSize = (BootAreaSize + HAMMER2_VOLUME_ALIGNMASK64) &
		        ~HAMMER2_VOLUME_ALIGNMASK64;
	opt->BootAreaSize = BootAreaSize;

	hammer2_off_t AuxAreaSize = opt->AuxAreaSize;
	if (AuxAreaSize == 0) {
		AuxAreaSize = HAMMER2_AUX_NOM_BYTES;
		while (AuxAreaSize > fso->total_size / 20)
			AuxAreaSize >>= 1;
		if (AuxAreaSize < HAMMER2_AUX_MIN_BYTES)
			AuxAreaSize = HAMMER2_AUX_MIN_BYTES;
	} else if (AuxAreaSize < HAMMER2_AUX_MIN_BYTES) {
		AuxAreaSize = HAMMER2_AUX_MIN_BYTES;
	}
	AuxAreaSize = (AuxAreaSize + HAMMER2_VOLUME_ALIGNMASK64) &
		       ~HAMMER2_VOLUME_ALIGNMASK64;
	opt->AuxAreaSize = AuxAreaSize;
}

int64_t
getsize(const char *str, int64_t minval, int64_t maxval, int powerof2)
{
	int64_t val;
	char *ptr;

	val = strtoll(str, &ptr, 0);
	switch(*ptr) {
	case 't':
	case 'T':
		val *= 1024;

	case 'g':
	case 'G':
		val *= 1024;

	case 'm':
	case 'M':
		val *= 1024;

	case 'k':
	case 'K':
		val *= 1024;
		break;
	default:
		errx(1, "Unknown suffix in number '%s'", str);

	}
	if (ptr[1]) {
		errx(1, "Unknown suffix in number '%s'", str);

	}
	if (val < minval) {
		errx(1, "Value too small: %s, min is %s",
		     str, sizetostr(minval));

	}
	if (val > maxval) {
		errx(1, "Value too large: %s, max is %s",
		     str, sizetostr(maxval));

	}
	if ((powerof2 & 1) && (val ^ (val - 1)) != ((val << 1) - 1)) {
		errx(1, "Value not power of 2: %s", str);

	}
	if ((powerof2 & 2) && (val & HAMMER2_NEWFS_ALIGNMASK)) {
		errx(1, "Value not an integral multiple of %dK: %s",
		     HAMMER2_NEWFS_ALIGN / 1024, str);

	}
	return(val);
}

static uint64_t
nowtime(void)
{
	struct timeval tv;
	uint64_t xtime;

	gettimeofday(&tv, NULL);
	xtime = tv.tv_sec * 1000000LL + tv.tv_usec;
	return(xtime);
}

static hammer2_off_t
format_hammer2_misc(hammer2_volume_t *vol, hammer2_mkfs_options_t *opt,
		    hammer2_off_t boot_base, hammer2_off_t aux_base)
{
	char *buf = malloc(HAMMER2_PBUFSIZE);
	hammer2_off_t alloc_base = aux_base + opt->AuxAreaSize;
	hammer2_off_t tmp_base;
	size_t n;
	int i;

	bzero(buf, HAMMER2_PBUFSIZE);
	tmp_base = 0;
	for (i = 0; i < HAMMER2_ZONE_BLOCKS_SEG; ++i) {
		n = pwrite(vol->fd, buf, HAMMER2_PBUFSIZE, tmp_base);
		if (n != HAMMER2_PBUFSIZE) {
			perror("write");
			exit(1);
		}
		tmp_base += HAMMER2_PBUFSIZE;
	}

	assert((alloc_base & HAMMER2_PBUFMASK) == 0);
	assert(alloc_base < HAMMER2_FREEMAP_LEVEL1_SIZE);

	for (tmp_base = boot_base; tmp_base < alloc_base;
	     tmp_base += HAMMER2_PBUFSIZE) {
		n = pwrite(vol->fd, buf, HAMMER2_PBUFSIZE, tmp_base);
		if (n != HAMMER2_PBUFSIZE) {
			perror("write (boot/aux)");
			exit(1);
		}
	}

	free(buf);
	return(alloc_base);
}

static hammer2_off_t
format_hammer2_inode(hammer2_volume_t *vol, hammer2_mkfs_options_t *opt,
		     hammer2_blockref_t *sroot_blockrefp,
		     hammer2_off_t alloc_base)
{
	char *buf = malloc(HAMMER2_PBUFSIZE);
	hammer2_inode_data_t *rawip;
	hammer2_blockref_t sroot_blockref;
	hammer2_blockref_t root_blockref[MAXLABELS];
	uint64_t now;
	size_t n;
	int i;

	bzero(buf, HAMMER2_PBUFSIZE);
	bzero(&sroot_blockref, sizeof(sroot_blockref));
	bzero(root_blockref, sizeof(root_blockref));
	now = nowtime();
	alloc_base &= ~HAMMER2_PBUFMASK64;
	alloc_direct(&alloc_base, &sroot_blockref, HAMMER2_INODE_BYTES);

	for (i = 0; i < opt->NLabels; ++i) {
		hammer2_uuid_create(&opt->Hammer2_PfsCLID[i]);
		hammer2_uuid_create(&opt->Hammer2_PfsFSID[i]);

		alloc_direct(&alloc_base, &root_blockref[i],
			     HAMMER2_INODE_BYTES);
		assert(((sroot_blockref.data_off ^ root_blockref[i].data_off) &
			~HAMMER2_PBUFMASK64) == 0);

		rawip = (void *)(buf + (HAMMER2_OFF_MASK_LO &
					root_blockref[i].data_off));
		rawip->meta.version = HAMMER2_INODE_VERSION_ONE;
		rawip->meta.ctime = now;
		rawip->meta.mtime = now;

		rawip->meta.btime = now;
		rawip->meta.type = HAMMER2_OBJTYPE_DIRECTORY;
		rawip->meta.mode = 0755;
		rawip->meta.inum = 1;
		rawip->meta.nlinks = 1;

		rawip->meta.name_len = strlen(opt->Label[i]);
		bcopy(opt->Label[i], rawip->filename, rawip->meta.name_len);
		rawip->meta.name_key =
				dirhash((char *)rawip->filename, rawip->meta.name_len);

		if (strcasecmp(opt->Label[i], "BOOT") == 0) {
			rawip->meta.comp_algo = HAMMER2_ENC_ALGO(
						    HAMMER2_COMP_AUTOZERO);
			rawip->meta.check_algo = HAMMER2_ENC_ALGO(
						    HAMMER2_CHECK_XXHASH64);
		} else {
			rawip->meta.comp_algo = HAMMER2_ENC_ALGO(
						    opt->CompType);
			rawip->meta.check_algo = HAMMER2_ENC_ALGO(
						    HAMMER2_CHECK_XXHASH64);
		}

		rawip->meta.pfs_clid = opt->Hammer2_PfsCLID[i];
		rawip->meta.pfs_fsid = opt->Hammer2_PfsFSID[i];
		rawip->meta.pfs_type = HAMMER2_PFSTYPE_MASTER;
		rawip->meta.op_flags |= HAMMER2_OPFLAG_PFSROOT;

		rawip->meta.pfs_inum = 16;

		root_blockref[i].key = rawip->meta.name_key;
		root_blockref[i].copyid = HAMMER2_COPYID_LOCAL;
		root_blockref[i].keybits = 0;
		root_blockref[i].check.xxhash64.value =
				XXH64(rawip, sizeof(*rawip), XXH_HAMMER2_SEED);
		root_blockref[i].type = HAMMER2_BREF_TYPE_INODE;
		root_blockref[i].methods =
				HAMMER2_ENC_CHECK(HAMMER2_CHECK_XXHASH64) |
				HAMMER2_ENC_COMP(HAMMER2_COMP_NONE);
		root_blockref[i].mirror_tid = 16;
		root_blockref[i].flags = HAMMER2_BREF_FLAG_PFSROOT;
	}

	rawip = (void *)(buf + (HAMMER2_OFF_MASK_LO & sroot_blockref.data_off));
	rawip->meta.version = HAMMER2_INODE_VERSION_ONE;
	rawip->meta.ctime = now;
	rawip->meta.mtime = now;

	rawip->meta.btime = now;
	rawip->meta.type = HAMMER2_OBJTYPE_DIRECTORY;
	rawip->meta.mode = 0700;
	rawip->meta.inum = 0;
	rawip->meta.nlinks = 2;

	rawip->meta.name_len = 0;
	rawip->meta.name_key = 0;

	rawip->meta.comp_algo = HAMMER2_ENC_ALGO(HAMMER2_COMP_AUTOZERO);
	rawip->meta.check_algo = HAMMER2_ENC_ALGO(HAMMER2_CHECK_XXHASH64);

	rawip->meta.pfs_clid = opt->Hammer2_SupCLID;
	rawip->meta.pfs_fsid = opt->Hammer2_SupFSID;
	rawip->meta.pfs_type = HAMMER2_PFSTYPE_SUPROOT;
	snprintf((char*)rawip->filename, sizeof(rawip->filename), "SUPROOT");
	rawip->meta.name_key = 0;
	rawip->meta.name_len = strlen((char*)rawip->filename);

	rawip->meta.pfs_inum = 0;

	qsort(root_blockref, opt->NLabels, sizeof(root_blockref[0]), blkrefary_cmp);
	for (i = 0; i < opt->NLabels; ++i)
		rawip->u.blockset.blockref[i] = root_blockref[i];

	sroot_blockref.copyid = HAMMER2_COPYID_LOCAL;
	sroot_blockref.keybits = 0;
	sroot_blockref.check.xxhash64.value =
				XXH64(rawip, sizeof(*rawip), XXH_HAMMER2_SEED);
	sroot_blockref.type = HAMMER2_BREF_TYPE_INODE;
	sroot_blockref.methods = HAMMER2_ENC_CHECK(HAMMER2_CHECK_XXHASH64) |
			         HAMMER2_ENC_COMP(HAMMER2_COMP_AUTOZERO);
	sroot_blockref.mirror_tid = 16;
	rawip = NULL;

	assert((sroot_blockref.data_off & ~HAMMER2_PBUFMASK64) ==
		((alloc_base - 1) & ~HAMMER2_PBUFMASK64));
	n = pwrite(vol->fd, buf, HAMMER2_PBUFSIZE,
		   sroot_blockref.data_off & ~HAMMER2_PBUFMASK64);
	if (n != HAMMER2_PBUFSIZE) {
		perror("write");
		exit(1);
	}
	*sroot_blockrefp = sroot_blockref;

	free(buf);
	return(alloc_base);
}

static void
format_hammer2(hammer2_ondisk_t *fso, hammer2_mkfs_options_t *opt, int index)
{
	char *buf = malloc(HAMMER2_PBUFSIZE);
	hammer2_volume_t *vol = &fso->volumes[index];
	hammer2_volume_data_t *voldata;
	hammer2_blockset_t sroot_blockset;
	hammer2_off_t boot_base = HAMMER2_ZONE_SEG;
	hammer2_off_t aux_base = boot_base + opt->BootAreaSize;
	hammer2_off_t alloc_base;
	size_t n;
	int i;

	bzero(buf, HAMMER2_PBUFSIZE);
	n = pwrite(vol->fd, buf, HAMMER2_PBUFSIZE,
		   vol->size - HAMMER2_PBUFSIZE);
	if (n != HAMMER2_PBUFSIZE) {
		perror("write (at-end-of-volume)");
		exit(1);
	}

	bzero(&sroot_blockset, sizeof(sroot_blockset));
	if (vol->id == HAMMER2_ROOT_VOLUME) {
		alloc_base = format_hammer2_misc(vol, opt, boot_base, aux_base);
		alloc_base = format_hammer2_inode(vol, opt,
						  &sroot_blockset.blockref[0],
						  alloc_base);
	} else {
		alloc_base = 0;
		for (i = 0; i < HAMMER2_SET_COUNT; ++i)
			sroot_blockset.blockref[i].type = HAMMER2_BREF_TYPE_INVALID;
	}

	assert(HAMMER2_VOLUME_BYTES <= HAMMER2_PBUFSIZE);
	bzero(buf, HAMMER2_PBUFSIZE);
	voldata = (void *)buf;

	voldata->magic = HAMMER2_VOLUME_ID_HBO;
	if (vol->id == HAMMER2_ROOT_VOLUME) {
		voldata->boot_beg = boot_base;
		voldata->boot_end = boot_base + opt->BootAreaSize;
		voldata->aux_beg = aux_base;
		voldata->aux_end = aux_base + opt->AuxAreaSize;
	}
	voldata->volu_size = vol->size;
	voldata->version = opt->Hammer2Version;
	voldata->flags = 0;

	if (voldata->version >= HAMMER2_VOL_VERSION_MULTI_VOLUMES) {
		voldata->volu_id = vol->id;
		voldata->nvolumes = fso->nvolumes;
		voldata->total_size = fso->total_size;
		for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
			if (i < fso->nvolumes)
				voldata->volu_loff[i] = fso->volumes[i].offset;
			else
				voldata->volu_loff[i] = (hammer2_off_t)-1;
		}
	}

	voldata->fsid = opt->Hammer2_VolFSID;
	voldata->fstype = opt->Hammer2_FSType;

	voldata->peer_type = DMSG_PEER_HAMMER2;

	assert(vol->id == HAMMER2_ROOT_VOLUME || alloc_base == 0);
	voldata->allocator_size = fso->free_size;
	if (vol->id == HAMMER2_ROOT_VOLUME) {
		voldata->allocator_free = fso->free_size;
		voldata->allocator_beg = alloc_base;
	}

	voldata->sroot_blockset = sroot_blockset;
	voldata->mirror_tid = 16;
	voldata->freemap_tid = 16;
	voldata->icrc_sects[HAMMER2_VOL_ICRC_SECT1] =
			hammer2_icrc32((char *)voldata + HAMMER2_VOLUME_ICRC1_OFF,
				       HAMMER2_VOLUME_ICRC1_SIZE);

	voldata->icrc_sects[HAMMER2_VOL_ICRC_SECT0] =
			hammer2_icrc32((char *)voldata + HAMMER2_VOLUME_ICRC0_OFF,
				       HAMMER2_VOLUME_ICRC0_SIZE);
	voldata->icrc_volheader =
			hammer2_icrc32((char *)voldata + HAMMER2_VOLUME_ICRCVH_OFF,
				       HAMMER2_VOLUME_ICRCVH_SIZE);

	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		if (i * HAMMER2_ZONE_BYTES64 >= vol->size)
			break;
		n = pwrite(vol->fd, buf, HAMMER2_PBUFSIZE,
			   i * HAMMER2_ZONE_BYTES64);
		if (n != HAMMER2_PBUFSIZE) {
			perror("write");
			exit(1);
		}
	}
	fsync(vol->fd);

	free(buf);
}

static void
alloc_direct(hammer2_off_t *basep, hammer2_blockref_t *bref, size_t bytes)
{
	int radix;

	radix = 0;
	assert(bytes);
	while ((bytes & 1) == 0) {
		bytes >>= 1;
		++radix;
	}
	assert(bytes == 1);
	if (radix < HAMMER2_RADIX_MIN)
		radix = HAMMER2_RADIX_MIN;

	bzero(bref, sizeof(*bref));
	bref->data_off = *basep | radix;
	bref->vradix = radix;

	*basep += 1U << radix;
}

static int
blkrefary_cmp(const void *b1, const void *b2)
{
	const hammer2_blockref_t *bref1 = b1;
	const hammer2_blockref_t *bref2 = b2;

	if (bref1->key < bref2->key)
		return(-1);
	if (bref1->key > bref2->key)
		return(1);
	return 0;
}

void
hammer2_mkfs(int ac, char **av, hammer2_mkfs_options_t *opt)
{
	hammer2_off_t resid = 0, reserved_size;
	hammer2_ondisk_t fso;
	int i;
	char *vol_fsid = NULL;
	char *sup_clid_name = NULL;
	char *sup_fsid_name = NULL;
	char *pfs_clid_name = NULL;
	char *pfs_fsid_name = NULL;

	assert(sizeof(hammer2_volume_data_t) == HAMMER2_VOLUME_BYTES);
	assert(sizeof(hammer2_inode_data_t) == HAMMER2_INODE_BYTES);
	assert(sizeof(hammer2_blockref_t) == HAMMER2_BLOCKREF_BYTES);

	hammer2_init_ondisk(&fso);
	fso.version = opt->Hammer2Version;
	fso.nvolumes = ac;

	assert(ac >= 1);
	if (opt->NFileSystemSizes == 1) {
		resid = opt->FileSystemSize[0];
		assert(resid >= HAMMER2_FREEMAP_LEVEL1_SIZE);
	} else if (opt->NFileSystemSizes > 1) {
		if (ac != opt->NFileSystemSizes)
			errx(1, "Invalid filesystem size count %d vs %d",
			    opt->NFileSystemSizes, ac);
	}

	for (i = 0; i < fso.nvolumes; ++i) {
		hammer2_volume_t *vol = &fso.volumes[i];
		hammer2_off_t size;
		int fd = open(av[i], O_RDWR);
		if (fd < 0)
			err(1, "Unable to open %s R+W", av[i]);
		size = check_volume(fd);

		if (opt->NFileSystemSizes == 1) {
			if (resid == 0)
				errx(1, "No remaining filesystem size for %s",
				    av[i]);
			if (size > resid)
				size = resid;
			resid -= size;
		} else if (opt->NFileSystemSizes > 1) {
			resid = opt->FileSystemSize[i];
			assert(resid >= HAMMER2_FREEMAP_LEVEL1_SIZE);
			if (size > resid)
				size = resid;
		}

		if (i == fso.nvolumes - 1)
			size &= ~HAMMER2_VOLUME_ALIGNMASK64;
		else
			size &= ~HAMMER2_FREEMAP_LEVEL1_MASK;
		if (size == 0)
			errx(1, "%s has aligned size of 0", av[i]);
		hammer2_install_volume(vol, fd, i, av[i], fso.total_size, size);
		fso.total_size += size;
	}

	for (i = 0; i < fso.nvolumes; ++i) {
		hammer2_volume_t *vol = &fso.volumes[i];
		printf("Volume %-15s size %s\n", vol->path,
		       sizetostr(vol->size));
	}
	hammer2_verify_volumes(&fso, NULL);

	adjust_options(&fso, opt);

	hammer2_uuid_to_str(&opt->Hammer2_VolFSID, &vol_fsid);
	hammer2_uuid_to_str(&opt->Hammer2_SupCLID, &sup_clid_name);
	hammer2_uuid_to_str(&opt->Hammer2_SupFSID, &sup_fsid_name);

	reserved_size = ((fso.total_size + HAMMER2_FREEMAP_LEVEL1_MASK) /
			  HAMMER2_FREEMAP_LEVEL1_SIZE) * HAMMER2_ZONE_SEG64;

	fso.free_size = fso.total_size - reserved_size - opt->BootAreaSize - opt->AuxAreaSize;
	if ((int64_t)fso.free_size < 0) {
		fprintf(stderr, "Not enough free space\n");
		exit(1);
	}

	for (i = 0; i < fso.nvolumes; ++i)
		format_hammer2(&fso, opt, i);

	printf("---------------------------------------------\n");
	printf("version:          %d\n", opt->Hammer2Version);
	printf("total-size:       %s (%jd bytes)\n",
	       sizetostr(fso.total_size),
	       (intmax_t)fso.total_size);
	printf("boot-area-size:   %s (%jd bytes)\n",
	       sizetostr(opt->BootAreaSize),
	       (intmax_t)opt->BootAreaSize);
	printf("aux-area-size:    %s (%jd bytes)\n",
	       sizetostr(opt->AuxAreaSize),
	       (intmax_t)opt->AuxAreaSize);
	printf("topo-reserved:    %s (%jd bytes)\n",
	       sizetostr(reserved_size),
	       (intmax_t)reserved_size);
	printf("free-size:        %s (%jd bytes)\n",
	       sizetostr(fso.free_size),
	       (intmax_t)fso.free_size);
	printf("vol-fsid:         %s\n", vol_fsid);
	printf("sup-clid:         %s\n", sup_clid_name);
	printf("sup-fsid:         %s\n", sup_fsid_name);
	for (i = 0; i < opt->NLabels; ++i) {
		printf("PFS \"%s\"\n", opt->Label[i]);
		hammer2_uuid_to_str(&opt->Hammer2_PfsCLID[i], &pfs_clid_name);
		hammer2_uuid_to_str(&opt->Hammer2_PfsFSID[i], &pfs_fsid_name);
		printf("    clid %s\n", pfs_clid_name);
		printf("    fsid %s\n", pfs_fsid_name);
	}
	if (opt->DebugOpt) {
		printf("---------------------------------------------\n");
		hammer2_print_volumes(&fso);
	}

	free(vol_fsid);
	free(sup_clid_name);
	free(sup_fsid_name);
	free(pfs_clid_name);
	free(pfs_fsid_name);

	for (i = 0; i < fso.nvolumes; ++i)
		hammer2_uninstall_volume(&fso.volumes[i]);
}
