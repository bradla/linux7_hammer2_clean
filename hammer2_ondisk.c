#include "hammer2.h"
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/string.h>

static char hammer2_bdev_holder;

int
hammer2_open_devvp(void *mp, const hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;
	struct file *bdev_file;
	int error = 0;
	int rdonly = 0;

	(void)mp;

	TAILQ_FOREACH(e, devvpl, entry) {
		blk_mode_t mode;
		int exclusive = 1;

		KKASSERT(e->path);
		mode = rdonly ? BLK_OPEN_READ : (BLK_OPEN_READ | BLK_OPEN_WRITE);

		bdev_file = bdev_file_open_by_path(e->path, mode,
		    &hammer2_bdev_holder, NULL);
		if (IS_ERR(bdev_file)) {

			exclusive = 0;
			bdev_file = bdev_file_open_by_path(e->path, mode,
			    NULL, NULL);
		}
		if (IS_ERR(bdev_file)) {
			error = PTR_ERR(bdev_file);
			hprintf("bdev_file_open_by_path(%s) failed: %d\n",
			    e->path, error);
			return error;
		}
		e->bdev_file = bdev_file;
		e->bdev = file_bdev(bdev_file);
		e->open = 1;

		if (exclusive) {
			error = set_blocksize(bdev_file, PAGE_SIZE);
			if (error)
				debug_hprintf("%s: set_blocksize(%lu) failed: "
				    "%d\n", e->path, PAGE_SIZE, error);
		}

		if (block_size(e->bdev) != PAGE_SIZE) {
			hprintf("%s: device block size is %u, need %lu -- "
			    "refusing to mount (exclusive open %s, logical "
			    "block size %u, device size %llu bytes).  A device "
			    "whose size is not a multiple of %lu is pinned "
			    "below PAGE_SIZE by set_init_blocksize(); "
			    "repartition to a size divisible by %lu.\n",
			    e->path, block_size(e->bdev), PAGE_SIZE,
			    exclusive ? "held" : "refused",
			    bdev_logical_block_size(e->bdev),
			    (unsigned long long)bdev_nr_bytes(e->bdev),
			    PAGE_SIZE, PAGE_SIZE);
			return EINVAL;
		}
	}

	return error;
}

int
hammer2_close_devvp(const hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;

	TAILQ_FOREACH(e, devvpl, entry) {
		if (e->open) {
			if (e->bdev_file)
				fput(e->bdev_file);
			e->bdev_file = NULL;
			e->bdev = NULL;
			e->open = 0;
		}
	}

	return 0;
}

int
hammer2_init_devvp(const void *mp, const char *blkdevs,
    hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;
	const char *p;
	char *path;
	size_t i, prefix_len;
	int error = 0;

	(void)mp;
	KKASSERT(blkdevs);
	p = blkdevs;

	path = hmalloc(PATH_MAX, M_TEMP, M_WAITOK | M_ZERO);
	if (!path)
		return -ENOMEM;

	while (1) {
		path[0] = '\0';
		if (*p != '/') {
			strscpy(path, "/dev/", PATH_MAX);
			prefix_len = strlen(path);
		} else {
			prefix_len = 0;
		}

		for (i = prefix_len; i < PATH_MAX - 1; ++i) {
			if (*p == '\0') {
				break;
			} else if (*p == ':') {
				p++;
				break;
			} else {
				path[i] = *p;
				p++;
			}
		}
		path[i] = '\0';

		if (strlen(path) <= strlen("/dev/")) {
			if (strlen(p)) {
				hprintf("ignore incomplete path %s\n", path);
				continue;
			}
			break;
		}

		e = hmalloc(sizeof(*e), M_HAMMER2, M_WAITOK | M_ZERO);
		if (!e) {
			error = -ENOMEM;
			break;
		}
		e->bdev_file = NULL;
		e->bdev = NULL;
		e->path = kstrdup(path, GFP_KERNEL);
		e->open = 0;
		TAILQ_INSERT_TAIL(devvpl, e, entry);
	}

	hfree(path, M_TEMP, PATH_MAX);
	return error;
}

void
hammer2_cleanup_devvp(hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;

	while (!list_empty(&devvpl->head)) {
		e = list_first_entry(&devvpl->head, hammer2_devvp_t, entry);
		list_del(&e->entry);

		if (e->bdev_file) {
			fput(e->bdev_file);
			e->bdev_file = NULL;
			e->bdev = NULL;
		}
		if (e->path) {
			kfree(e->path);
			e->path = NULL;
		}
		hfree(e, M_HAMMER2, sizeof(*e));
	}
}

static int
hammer2_verify_volumes_common(const hammer2_volume_t *volumes,
			      const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	const char *path;
	loff_t mediasize;
	int i;

	if (rootvoldata->volu_id != HAMMER2_ROOT_VOLUME) {
		hprintf("volume id %d must be %d\n", rootvoldata->volu_id,
		    HAMMER2_ROOT_VOLUME);
		return EINVAL;
	}

	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id == -1)
			continue;
		path = vol->dev->path;

		if (!vol->dev->bdev) {
			hprintf("%s has NULL bdev\n", path);
			return EINVAL;
		}
		if (vol->offset == (hammer2_off_t)-1) {
			hprintf("%s has bad offset %016llx\n",
			    path, (long long)vol->offset);
			return EINVAL;
		}
		if (vol->size == (hammer2_off_t)-1) {
			hprintf("%s has bad size %016llx\n",
			    path, (long long)vol->size);
			return EINVAL;
		}

		mediasize = bdev_nr_bytes(vol->dev->bdev);
		if ((hammer2_off_t)vol->size > (hammer2_off_t)mediasize) {
			hprintf("%s's size %016llx exceeds device size %016llx\n",
			    path, (long long)vol->size,
			    (long long)mediasize);
			return EINVAL;
		}
		if (vol->size == 0) {
			hprintf("%s has size of 0\n", path);
			return EINVAL;
		}
	}

	return 0;
}

static int
hammer2_verify_volumes_1(const hammer2_volume_t *volumes,
    const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	hammer2_off_t off;
	const char *path;
	int i, nvolumes = 0;

	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id != -1)
			nvolumes++;
	}
	if (nvolumes != 1) {
		hprintf("only 1 volume supported\n");
		return (EINVAL);
	}

	if (rootvoldata->nvolumes) {
		hprintf("volume count %d must be 0\n", rootvoldata->nvolumes);
		return (EINVAL);
	}
	if (rootvoldata->total_size) {
		hprintf("total size %016llx must be 0\n",
		    (long long)rootvoldata->total_size);
		return (EINVAL);
	}
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off) {
			hprintf("volume offset[%d] %016llx must be 0\n",
			    i, (long long)off);
			return (EINVAL);
		}
	}

	vol = &volumes[HAMMER2_ROOT_VOLUME];
	path = vol->dev->path;
	if (vol->id) {
		hprintf("%s has non zero id %d\n", path, vol->id);
		return (EINVAL);
	}
	if (vol->offset) {
		hprintf("%s has non zero offset %016llx\n",
		    path, (long long)vol->offset);
		return (EINVAL);
	}
	if (vol->size & HAMMER2_VOLUME_ALIGNMASK64) {
		hprintf("%s's size is not %016llx aligned\n",
		    path, (long long)HAMMER2_VOLUME_ALIGN);
		return (EINVAL);
	}

	return (0);
}

static int
hammer2_verify_volumes_2(const hammer2_volume_t *volumes,
    const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	hammer2_off_t off, total_size = 0;
	const char *path;
	int i, nvolumes = 0;

	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id != -1) {
			nvolumes++;
			total_size += vol->size;
		}
	}

	if (rootvoldata->nvolumes != nvolumes) {
		hprintf("volume header requires %d devices, %d specified\n",
		    rootvoldata->nvolumes, nvolumes);
		return (EINVAL);
	}
	if (rootvoldata->total_size != total_size) {
		hprintf("total size %016llx does not equal sum of volumes "
		    "%016llx\n",
		    (long long)rootvoldata->total_size, (long long)total_size);
		return (EINVAL);
	}
	for (i = 0; i < nvolumes; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off == (hammer2_off_t)-1) {
			hprintf("volume offset[%d] %016llx must not be -1\n",
			    i, (long long)off);
			return (EINVAL);
		}
	}
	for (i = nvolumes; i < HAMMER2_MAX_VOLUMES; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off != (hammer2_off_t)-1) {
			hprintf("volume offset[%d] %016llx must be -1\n",
			    i, (long long)off);
			return (EINVAL);
		}
	}

	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id == -1)
			continue;
		path = vol->dev->path;

		if (vol->offset & HAMMER2_FREEMAP_LEVEL1_MASK) {
			hprintf("%s's offset %016llx not %016llx aligned\n",
			    path, (long long)vol->offset,
			    (long long)HAMMER2_FREEMAP_LEVEL1_SIZE);
			return (EINVAL);
		}

		if (i) {
			if (vol->id <= (vol-1)->id) {
				hprintf("%s has inconsistent id %d\n",
				    path, vol->id);
				return (EINVAL);
			}
			if (vol->offset != (vol-1)->offset + (vol-1)->size) {
				hprintf("%s has inconsistent offset %016llx\n",
				    path, (long long)vol->offset);
				return (EINVAL);
			}
		} else {
			if (vol->offset) {
				hprintf("%s has non zero offset %016llx\n",
				    path, (long long)vol->offset);
				return (EINVAL);
			}
		}

		if (i != rootvoldata->nvolumes - 1) {
			if (vol->size < HAMMER2_FREEMAP_LEVEL1_SIZE) {
				hprintf("%s's size must be >= %016llx\n",
				    path,
				    (long long)HAMMER2_FREEMAP_LEVEL1_SIZE);
				return (EINVAL);
			}
			if (vol->size & HAMMER2_FREEMAP_LEVEL1_MASK) {
				hprintf("%s's size is not %016llx aligned\n",
				    path,
				    (long long)HAMMER2_FREEMAP_LEVEL1_SIZE);
				return (EINVAL);
			}
		} else {
			if (vol->size & HAMMER2_VOLUME_ALIGNMASK64) {
				hprintf("%s's size is not %016llx aligned\n",
				    path, (long long)HAMMER2_VOLUME_ALIGN);
				return (EINVAL);
			}
		}
	}

	return (0);
}

static int
hammer2_verify_volumes(const hammer2_volume_t *volumes,
    const hammer2_volume_data_t *rootvoldata)
{
	int error;

	error = hammer2_verify_volumes_common(volumes, rootvoldata);
	if (error)
		return (error);

	if (rootvoldata->version >= HAMMER2_VOL_VERSION_MULTI_VOLUMES)
		return (hammer2_verify_volumes_2(volumes, rootvoldata));
	else
		return (hammer2_verify_volumes_1(volumes, rootvoldata));
}

static int
hammer2_read_volume_header(struct block_device *bdev, const char *path,
    hammer2_volume_data_t *voldata)
{
	hammer2_volume_data_t *vd;
	hammer2_crc32_t crc0, crc1;
	char *vbuf;
	loff_t mediasize;
	off_t blkoff;
	int i, zone = -1;

	KASSERTMSG(bdev != NULL, "NULL bdev");
	mediasize = bdev_nr_bytes(bdev);

	vbuf = kvmalloc(HAMMER2_VOLUME_BYTES, GFP_KERNEL);
	if (!vbuf)
		return (-ENOMEM);

	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		blkoff = (off_t)i * HAMMER2_ZONE_BYTES64;
		if (blkoff >= mediasize)
			continue;

		if (hammer2_dev_bread(bdev, blkoff, vbuf, HAMMER2_VOLUME_BYTES))
			continue;

		vd = (struct hammer2_volume_data *)vbuf;

		if ((vd->magic != HAMMER2_VOLUME_ID_HBO) &&
		    (vd->magic != HAMMER2_VOLUME_ID_ABO)) {
			hprintf("%s #%d: bad magic\n", path, i);
			continue;
		}
		if (vd->magic == HAMMER2_VOLUME_ID_ABO) {

			hprintf("%s #%d: reverse-endian filesystem detected\n",
			    path, i);
			continue;
		}

		crc0 = vd->icrc_sects[HAMMER2_VOL_ICRC_SECT0];
		crc1 = hammer2_icrc32(vbuf + HAMMER2_VOLUME_ICRC0_OFF,
		    HAMMER2_VOLUME_ICRC0_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch sect0 "
			    "%08x/%08x\n",
			    path, i, crc0, crc1);
			continue;
		}
		crc0 = vd->icrc_sects[HAMMER2_VOL_ICRC_SECT1];
		crc1 = hammer2_icrc32(vbuf + HAMMER2_VOLUME_ICRC1_OFF,
		    HAMMER2_VOLUME_ICRC1_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch sect1 "
			    "%08x/%08x\n",
			    path, i, crc0, crc1);
			continue;
		}
		crc0 = vd->icrc_volheader;
		crc1 = hammer2_icrc32(vbuf + HAMMER2_VOLUME_ICRCVH_OFF,
		    HAMMER2_VOLUME_ICRCVH_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch vh "
			    "%08x/%08x\n",
			    path, i, crc0, crc1);
			continue;
		}

		if (zone == -1 || voldata->mirror_tid < vd->mirror_tid) {
			*voldata = *vd;
			zone = i;
		}
	}

	kvfree(vbuf);

	if (zone == -1) {
		hprintf("%s has no valid volume headers\n", path);
		return (-EINVAL);
	}
	return (zone);
}

static void
hammer2_print_uuid_mismatch(struct uuid *uuid1, struct uuid *uuid2,
    const char *id)
{
	hprintf("volume %s uuid mismatch (per-byte compare)\n", id);
	(void)uuid1;
	(void)uuid2;
}

int
hammer2_init_volumes(const hammer2_devvp_list_t *devvpl,
    hammer2_volume_t *volumes, hammer2_volume_data_t *rootvoldata,
    int *rootvolzone, struct block_device **rootvoldevvp)
{
	hammer2_volume_data_t *voldata;
	hammer2_volume_t *vol;
	hammer2_devvp_t *e;
	struct block_device *bdev;
	struct uuid fsid, fstype;
	const char *path;
	int i, zone, error = 0, v = -1, nvolumes = 0;

	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		vol->dev = NULL;
		vol->id = -1;
		vol->offset = (hammer2_off_t)-1;
		vol->size = (hammer2_off_t)-1;
	}

	voldata = hmalloc(sizeof(*voldata), M_HAMMER2, M_WAITOK | M_ZERO);
	bzero(&fsid, sizeof(fsid));
	bzero(&fstype, sizeof(fstype));
	bzero(rootvoldata, sizeof(*rootvoldata));

	TAILQ_FOREACH(e, devvpl, entry) {
		bdev = e->bdev;
		path = e->path;
		KKASSERT(bdev);

		error = hammer2_read_volume_header(bdev, path, voldata);
		if (error < 0) {
			hprintf("failed to read %s's volume header\n", path);
			error = -error;
			goto done;
		}
		zone = error;
		error = 0;

		if (voldata->volu_id >= HAMMER2_MAX_VOLUMES) {
			hprintf("%s has bad volume id %d\n",
			    path, voldata->volu_id);
			error = EINVAL;
			goto done;
		}
		vol = &volumes[voldata->volu_id];
		if (vol->id != -1) {
			hprintf("volume id %d already initialized\n",
			    voldata->volu_id);
			error = EINVAL;
			goto done;
		}

		if (v == -1) {
			v = voldata->version;
			nvolumes = voldata->nvolumes;
			fsid = voldata->fsid;
			fstype = voldata->fstype;
		} else {
			if (v != (int)voldata->version) {
				hprintf("volume version mismatch %d vs %d\n",
				    v, (int)voldata->version);
				error = ENXIO;
				goto done;
			}
			if (nvolumes != voldata->nvolumes) {
				hprintf("volume count mismatch %d vs %d\n",
				    nvolumes, voldata->nvolumes);
				error = ENXIO;
				goto done;
			}
			if (bcmp(&fsid, &voldata->fsid, sizeof(fsid))) {
				hammer2_print_uuid_mismatch(&fsid,
				    &voldata->fsid, "fsid");
				error = ENXIO;
				goto done;
			}
			if (bcmp(&fstype, &voldata->fstype, sizeof(fstype))) {
				hammer2_print_uuid_mismatch(&fstype,
				    &voldata->fstype, "fstype");
				error = ENXIO;
				goto done;
			}
		}
		if (v < HAMMER2_VOL_VERSION_MIN ||
		    v > HAMMER2_VOL_VERSION_WIP) {
			hprintf("bad volume version %d\n", v);
			error = EINVAL;
			goto done;
		}

		vol->dev = e;
		vol->id = voldata->volu_id;
		vol->offset = voldata->volu_loff[vol->id];
		vol->size = voldata->volu_size;
		if (vol->id == HAMMER2_ROOT_VOLUME) {
			bcopy(voldata, rootvoldata, sizeof(*rootvoldata));
			*rootvolzone = zone;
			KKASSERT(*rootvoldevvp == NULL);
			*rootvoldevvp = bdev;
		}
		debug_hprintf("\"%s\" zone %d id %d offset %016llx size "
		    "%016llx\n",
		    path, zone, vol->id, (long long)vol->offset,
		    (long long)vol->size);
	}
done:
	if (error == 0) {
		if (!rootvoldata->version) {
			hprintf("root volume not found\n");
			error = EINVAL;
		}
		if (error == 0)
			error = hammer2_verify_volumes(volumes, rootvoldata);
	}
	hfree(voldata, M_HAMMER2, sizeof(*voldata));

	return (error);
}

hammer2_volume_t*
hammer2_get_volume(hammer2_dev_t *hmp, hammer2_off_t offset)
{
	hammer2_volume_t *vol, *ret = NULL;
	int i;

	offset &= ~HAMMER2_OFF_MASK_RADIX;

	for (i = 0; i < hmp->nvolumes; ++i) {
		vol = &hmp->volumes[i];
		if ((offset >= vol->offset) &&
		    (offset < vol->offset + vol->size)) {
			ret = vol;
			break;
		}
	}

	if (!ret)
		hpanic("no volume for offset %016llx", (long long)offset);

	KKASSERT(ret);
	KKASSERT(ret->dev);
	KKASSERT(ret->dev->path);

	return ret;
}

int
hammer2_access_devvp(struct block_device *bdev, int rdonly)
{
	(void)bdev;
	(void)rdonly;
	return 0;
}

int
hammer2_getw_devvp(struct block_device *bdev)
{
	(void)bdev;
	return 0;
}

int
hammer2_putw_devvp(struct block_device *bdev)
{
	(void)bdev;
	return 0;
}
