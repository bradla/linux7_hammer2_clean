#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <err.h>
#include <uuid/uuid.h>

#include <vfs/hammer2/hammer2_disk.h>
#include <vfs/hammer2/hammer2_ioctl.h>

#include "hammer2_subs.h"

int
hammer2_ioctl_handle(const char *sel_path)
{
	struct hammer2_ioc_version info;
	int fd;

	if (sel_path == NULL)
		sel_path = ".";

	fd = open(sel_path, O_RDONLY, 0);
	if (fd < 0) {
		fprintf(stderr, "hammer2: Unable to open %s: %s\n",
			sel_path, strerror(errno));
		return(-1);
	}
	if (ioctl(fd, HAMMER2IOC_VERSION_GET, &info) < 0) {
		fprintf(stderr, "hammer2: '%s' is not a hammer2 filesystem\n",
			sel_path);
		close(fd);
		return(-1);
	}
	return (fd);
}

const char *
hammer2_time64_to_str(uint64_t htime64, char **strp)
{
	struct tm *tp;
	time_t t;

	if (*strp) {
		free(*strp);
		*strp = NULL;
	}
	*strp = malloc(64);
	t = htime64 / 1000000;
	tp = localtime(&t);
	strftime(*strp, 64, "%d-%b-%Y %H:%M:%S", tp);
	return (*strp);
}

const char *
hammer2_uuid_to_str(const hammer2_uuid_t *uuid, char **strp)
{
	if (*strp) {
		free(*strp);
		*strp = NULL;
	}
	hammer2_uuid_to_string(uuid, strp);
	return (*strp);
}

const char *
hammer2_iptype_to_str(uint8_t type)
{
	switch(type) {
	case HAMMER2_OBJTYPE_UNKNOWN:
		return("UNKNOWN");
	case HAMMER2_OBJTYPE_DIRECTORY:
		return("DIR");
	case HAMMER2_OBJTYPE_REGFILE:
		return("FILE");
	case HAMMER2_OBJTYPE_FIFO:
		return("FIFO");
	case HAMMER2_OBJTYPE_CDEV:
		return("CDEV");
	case HAMMER2_OBJTYPE_BDEV:
		return("BDEV");
	case HAMMER2_OBJTYPE_SOFTLINK:
		return("SOFTLINK");
	case HAMMER2_OBJTYPE_SOCKET:
		return("SOCKET");
	case HAMMER2_OBJTYPE_WHITEOUT:
		return("WHITEOUT");
	default:
		return("ILLEGAL");
	}
}

const char *
hammer2_pfstype_to_str(uint8_t type)
{
	switch(type) {
	case HAMMER2_PFSTYPE_NONE:
		return("NONE");
	case HAMMER2_PFSTYPE_SUPROOT:
		return("SUPROOT");
	case HAMMER2_PFSTYPE_DUMMY:
		return("DUMMY");
	case HAMMER2_PFSTYPE_CACHE:
		return("CACHE");
	case HAMMER2_PFSTYPE_SLAVE:
		return("SLAVE");
	case HAMMER2_PFSTYPE_SOFT_SLAVE:
		return("SOFT_SLAVE");
	case HAMMER2_PFSTYPE_SOFT_MASTER:
		return("SOFT_MASTER");
	case HAMMER2_PFSTYPE_MASTER:
		return("MASTER");
	default:
		return("ILLEGAL");
	}
}

const char *
hammer2_pfssubtype_to_str(uint8_t subtype)
{
	switch(subtype) {
	case HAMMER2_PFSSUBTYPE_NONE:
		return("NONE");
	case HAMMER2_PFSSUBTYPE_SNAPSHOT:
		return("SNAPSHOT");
	case HAMMER2_PFSSUBTYPE_AUTOSNAP:
		return("AUTOSNAP");
	default:
		return("ILLEGAL");
	}
}

const char *
hammer2_breftype_to_str(uint8_t type)
{
	switch(type) {
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

const char *
sizetostr(hammer2_off_t size)
{
	static char buf[32];

	if (size < 1024 / 2) {
		snprintf(buf, sizeof(buf), "%6.2fB", (double)size);
	} else if (size < 1024 * 1024 / 2) {
		snprintf(buf, sizeof(buf), "%6.2fKB",
			(double)size / 1024);
	} else if (size < 1024 * 1024 * 1024LL / 2) {
		snprintf(buf, sizeof(buf), "%6.2fMB",
			(double)size / (1024 * 1024));
	} else if (size < 1024 * 1024 * 1024LL * 1024LL / 2) {
		snprintf(buf, sizeof(buf), "%6.2fGB",
			(double)size / (1024 * 1024 * 1024LL));
	} else {
		snprintf(buf, sizeof(buf), "%6.2fTB",
			(double)size / (1024 * 1024 * 1024LL * 1024LL));
	}
	return(buf);
}

const char *
counttostr(hammer2_off_t size)
{
	static char buf[32];

	if (size < 1024 / 2) {
		snprintf(buf, sizeof(buf), "%jd",
			 (intmax_t)size);
	} else if (size < 1024 * 1024 / 2) {
		snprintf(buf, sizeof(buf), "%jd",
			 (intmax_t)size);
	} else if (size < 1024 * 1024 * 1024LL / 2) {
		snprintf(buf, sizeof(buf), "%6.2fM",
			 (double)size / (1024 * 1024));
	} else if (size < 1024 * 1024 * 1024LL * 1024LL / 2) {
		snprintf(buf, sizeof(buf), "%6.2fG",
			 (double)(size / (1024 * 1024 * 1024LL)));
	} else {
		snprintf(buf, sizeof(buf), "%6.2fT",
			 (double)(size / (1024 * 1024 * 1024LL * 1024LL)));
	}
	return(buf);
}

hammer2_off_t
check_volume(int fd)
{
	struct stat st;
	hammer2_off_t size;

	if (ioctl(fd, BLKGETSIZE, &size) < 0) {

		if (fstat(fd, &st) < 0)
			err(1, "Unable to stat fd %d", fd);
		if (!S_ISREG(st.st_mode))
			errx(1, "Unsupported file type for fd %d", fd);
		size = st.st_size;
	} else {
		int sector_size;
		if (ioctl(fd, BLKSSZGET, &sector_size) < 0) {
			errx(1, "Failed to get media sector size");

		}
		if (sector_size > HAMMER2_PBUFSIZE ||
		    HAMMER2_PBUFSIZE % sector_size) {
			errx(1, "A media sector size of %d is not supported",
			     sector_size);
		}
		size <<= 9;
	}
	return(size);
}

hammer2_key_t
dirhash(const char *aname, size_t len)
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

char **
get_hammer2_mounts(int *acp)
{

	FILE *fp;
	char **av = NULL;
	char line[2048];
	int n = 0, cap = 0;

	*acp = 0;
	fp = fopen("/proc/self/mounts", "r");
	if (fp == NULL)
		return NULL;

	while (fgets(line, sizeof(line), fp) != NULL) {
		char *save = NULL;
		char *dev, *mnt, *type;

		dev = strtok_r(line, " ", &save);
		mnt = strtok_r(NULL, " ", &save);
		type = strtok_r(NULL, " ", &save);
		if (dev == NULL || mnt == NULL || type == NULL)
			continue;
		if (strcmp(type, "hammer2") != 0)
			continue;
		if (n >= cap) {
			char **nav;
			cap = cap ? cap * 2 : 8;
			nav = realloc(av, cap * sizeof(char *));
			if (nav == NULL)
				break;
			av = nav;
		}
		av[n++] = strdup(mnt);
	}
	fclose(fp);

	*acp = n;
	return av;
}

void
put_hammer2_mounts(int ac, char **av)
{
	if (av == NULL)
		return;
	while (--ac >= 0)
		free(av[ac]);
	free(av);
}
