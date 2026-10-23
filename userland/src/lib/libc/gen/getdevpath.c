#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <paths.h>
#include <limits.h>
#include <fstab.h>

#include "./util.h"

static void finddevlabel(char **pathp, const char *devname);
static int xlatedevpath(char **pathp, struct stat *st);
static char *dodequote(char *base);

char *
getdevpath(const char *devname, int flags)
{
	struct stat st;
	char *path = NULL;
	int stgood = 0;

	if (devname[0] == '/' || devname[0] == '.') {
		asprintf(&path, "%s", devname);
	} else {
		asprintf(&path, "/dev/%s", devname);
		if (lstat(path, &st) < 0) {
			free(path);
			path = NULL;
			finddevlabel(&path, devname);
			if (path == NULL)
				asprintf(&path, "%s", devname);
		} else {
			stgood = 1;
		}
	}

	if ((flags & GETDEVPATH_RAWDEV) && path) {
		if (stgood == 0 && lstat(path, &st) == 0)
			stgood = 1;
		if (stgood)
			stgood = xlatedevpath(&path, &st);
		if (stgood == 0) {
			free(path);
			path = NULL;
		}

	}
	if (path == NULL)
		errno = ENOENT;
	return(path);
}

static void
finddevlabel(char **pathp, const char *devname)
{
	const char *prefix = _PATH_DEVTAB_PATHS;
	const char *ptr1;
	const char *trailer;
	char *label;
	char *ptr2;
	char *ptr3;
	char *dtpath;
	char *bufp;
	char buf[256];
	FILE *fp;
	size_t len;
	size_t tlen;

	if ((trailer = strrchr(devname, '.')) != NULL)
		tlen = trailer - devname;
	else
		tlen = 0;

	while (*prefix && *pathp == NULL) {

		ptr1 = strchr(prefix, ':');
		len = (ptr1) ? (size_t)(ptr1 - prefix) : strlen(prefix);
		asprintf(&dtpath, "%*.*s/devtab", (int)len, (int)len, prefix);

		if ((fp = fopen(dtpath, "r")) != NULL) {
			while (fgets(buf, sizeof(buf), fp) != NULL) {

				label = strtok_r(buf, " \t\r\n", &bufp);
				if (label == NULL || *label == 0 ||
				    *label == '#') {
					continue;
				}

				if (strcmp(devname, label) == 0) {
					trailer = "";
				} else if (tlen && strlen(label) == tlen &&
					   strncmp(devname, label, tlen) == 0) {
					trailer = devname + tlen;
				} else {
					continue;
				}

				ptr2 = strtok_r(NULL, " \t\r\n", &bufp);
				ptr3 = strtok_r(NULL, " \t\r\n", &bufp);
				if (ptr2 == NULL || ptr3 == NULL)
					continue;
				if (*ptr2 == 0 || *ptr3 == 0)
					continue;
				ptr3 = dodequote(ptr3);
				if (strcmp(ptr2, "path") == 0) {
					asprintf(pathp, "%s%s", ptr3, trailer);
				} else {
					asprintf(pathp, "/dev/%s/%s%s",
						 ptr2, ptr3, trailer);
				}
				break;
			}
			fclose(fp);
		}
		free(dtpath);
		prefix += len;
		if (*prefix == ':')
			++prefix;
	}
}

static int
xlatedevpath(char **pathp, struct stat *st)
{
	char *path;
	int n;
	int len;

	if (!S_ISLNK(st->st_mode))
		return(1);

	len = (int)st->st_size;
	if (len < 0 || len > PATH_MAX)
		return(0);

	path = malloc(len + 1);
	n = readlink(*pathp, path, len);
	if (n < 0 || n > len) {
		free(path);
		return(0);
	}

	path[n] = 0;
	free(*pathp);
	*pathp = path;
	return(1);
}

static char *
dodequote(char *base)
{
	int len = strlen(base);

	if (len && base[0] == '\"' && base[len-1] == '\"') {
		base[len - 1] = 0;
		++base;
	}
	return(base);
}
