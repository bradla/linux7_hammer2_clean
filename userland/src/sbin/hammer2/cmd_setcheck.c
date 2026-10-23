#include "hammer2.h"

static int cmd_setcheck_core(uint8_t check_algo, const char *path_str,
			    struct stat *st);

int
cmd_setcheck(const char *check_str, char **paths)
{
	static const char *checks[] = HAMMER2_CHECK_STRINGS;
	struct stat st;
	int check_algo;
	int ecode;
	int res;

	ecode = 0;

	if (isdigit(check_str[0])) {
		check_algo = strtol(check_str, NULL, 0);
	} else {
		check_algo = HAMMER2_CHECK_STRINGS_COUNT;
		while (--check_algo >= 0) {
			if (strcasecmp(check_str, checks[check_algo]) == 0)
				break;
		}
		if (check_algo < 0 && strcasecmp(check_str, "default") == 0) {
			check_algo = HAMMER2_CHECK_XXHASH64;
			check_str = "xxhash64";
		}
		if (check_algo < 0 && strcasecmp(check_str, "disabled") == 0) {
			check_algo = HAMMER2_CHECK_DISABLED;
			check_str = "disabled";
		}
		if (check_algo < 0) {
			fprintf(stderr,
				"Unknown check code type: %s\n",
				check_str);
			ecode = 3;
		}
	}

	if (ecode == 0) {
		while (*paths) {
			if (lstat(*paths, &st) == 0) {
				res = cmd_setcheck_core(
					HAMMER2_ENC_ALGO(check_algo),
				        *paths,
					&st);
				if (res)
					ecode = res;
			} else {
				printf("%s: %s\n", *paths, strerror(errno));
				ecode = 3;
			}
			++paths;
		}
	}

	return ecode;
}

static int
cmd_setcheck_core(uint8_t check_algo, const char *path_str, struct stat *st)
{
	hammer2_ioc_inode_t inode;
	int fd;
	int res;

	fd = hammer2_ioctl_handle(path_str);
	if (fd < 0) {
		res = 3;
		goto failed;
	}
	res = ioctl(fd, HAMMER2IOC_INODE_GET, &inode);
	if (res < 0) {
		fprintf(stderr,
			"%s: HAMMER2IOC_INODE_GET: error %s\n",
			path_str, strerror(errno));
		res = 3;
		goto failed;
	}
	printf("%s\tcheck_algo=0x%02x\n", path_str, check_algo);
	inode.flags |= HAMMER2IOC_INODE_FLAG_CHECK;
	inode.ip_data.meta.check_algo = check_algo;
	res = ioctl(fd, HAMMER2IOC_INODE_SET, &inode);
	if (res < 0) {
		fprintf(stderr,
			"%s: HAMMER2IOC_INODE_SET: error %s\n",
			path_str, strerror(errno));
		res = 3;
		goto failed;
	}
	res = 0;

	if (RecurseOpt && S_ISDIR(st->st_mode)) {
		DIR *dir;
		char *path;
		struct dirent *den;

		if ((dir = fdopendir(fd)) != NULL) {
			while ((den = readdir(dir)) != NULL) {
				if (strcmp(den->d_name, ".") == 0 ||
				    strcmp(den->d_name, "..") == 0) {
					continue;
				}
				asprintf(&path, "%s/%s", path_str, den->d_name);
				if (lstat(path, st) == 0)
					cmd_setcheck_core(check_algo, path, st);
				free(path);
			}
			closedir(dir);
		}
	}
failed:
	close(fd);
	return res;
}
