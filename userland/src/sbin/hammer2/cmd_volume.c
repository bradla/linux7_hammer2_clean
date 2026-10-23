#include "hammer2.h"

int
cmd_volume_list(int ac, char **av)
{
	hammer2_ioc_volume_list_t vollist;
	hammer2_ioc_volume_t *entry;
	int fd, i, j, n, w, all = 0, ecode = 0;

	if (ac == 1 && av[0] == NULL) {
		av = get_hammer2_mounts(&ac);
		all = 1;
	}
	vollist.volumes = calloc(HAMMER2_MAX_VOLUMES, sizeof(*vollist.volumes));

	for (i = 0; i < ac; ++i) {
		if (i)
			printf("\n");
		if (ac > 1 || all)
			printf("%s\n", av[i]);
		if ((fd = hammer2_ioctl_handle(av[i])) < 0) {
			ecode = 1;
			goto failed;
		}

		vollist.nvolumes = HAMMER2_MAX_VOLUMES;
		if (ioctl(fd, HAMMER2IOC_VOLUME_LIST, &vollist) < 0) {
			perror("ioctl");
			close(fd);
			ecode = 1;
			goto failed;
		}

		w = 0;
		for (j = 0; j < vollist.nvolumes; ++j) {
			entry = &vollist.volumes[j];
			n = (int)strlen(entry->path);
			if (n > w)
				w = n;
		}

		if (QuietOpt > 0) {
			for (j = 0; j < vollist.nvolumes; ++j) {
				entry = &vollist.volumes[j];
				printf("%s\n", entry->path);
			}
		} else {
			printf("version %d\n", vollist.version);
			printf("@%s\n", vollist.pfs_name);
			for (j = 0; j < vollist.nvolumes; ++j) {
				entry = &vollist.volumes[j];
				printf("volume%-2d %-*.*s %s",
				       entry->id, w, w, entry->path,
				       sizetostr(entry->size));
				if (VerboseOpt > 0)
					printf(" 0x%016jx 0x%016jx",
					       (intmax_t)entry->offset,
					       (intmax_t)entry->size);
				printf("\n");
			}
		}
		close(fd);
	}
failed:
	free(vollist.volumes);
	if (all)
		put_hammer2_mounts(ac, av);

	return (ecode);
}
