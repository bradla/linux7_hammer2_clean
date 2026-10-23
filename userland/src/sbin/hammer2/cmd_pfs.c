#include "hammer2.h"

struct pfs_entry {
	TAILQ_ENTRY(pfs_entry) entry;
	char name[NAME_MAX+1];
	char s[NAME_MAX+1];
};

int
cmd_pfs_list(int ac, char **av)
{
	hammer2_ioc_pfs_t pfs;
	int ecode = 0;
	int fd;
	int i;
	int all = 0;
	char *pfs_id_str = NULL;
	const char *type_str;
	TAILQ_HEAD(, pfs_entry) head;
	struct pfs_entry *p, *e;

	if (ac == 1 && av[0] == NULL) {
		av = get_hammer2_mounts(&ac);
		all = 1;
	}

	for (i = 0; i < ac; ++i) {
		if ((fd = hammer2_ioctl_handle(av[i])) < 0)
			return(1);
		bzero(&pfs, sizeof(pfs));
		TAILQ_INIT(&head);
		if (i)
			printf("\n");

		while ((pfs.name_key = pfs.name_next) != (hammer2_key_t)-1) {
			if (ioctl(fd, HAMMER2IOC_PFS_GET, &pfs) < 0) {
				perror("ioctl");
				ecode = 1;
				break;
			}
			hammer2_uuid_to_str(&pfs.pfs_clid, &pfs_id_str);
			if (pfs.pfs_type == HAMMER2_PFSTYPE_MASTER) {
				if (pfs.pfs_subtype == HAMMER2_PFSSUBTYPE_NONE)
					type_str = "MASTER";
				else
					type_str = hammer2_pfssubtype_to_str(
						pfs.pfs_subtype);
			} else {
				type_str = hammer2_pfstype_to_str(pfs.pfs_type);
			}
			e = calloc(1, sizeof(*e));
			snprintf(e->name, sizeof(e->name), "%s", pfs.name);
			snprintf(e->s, sizeof(e->s), "%-11s %s",
				type_str, pfs_id_str);
			free(pfs_id_str);
			pfs_id_str = NULL;

			p = TAILQ_FIRST(&head);
			while (p) {
				if (strcmp(e->name, p->name) <= 0) {
					TAILQ_INSERT_BEFORE(p, e, entry);
					break;
				}
				p = TAILQ_NEXT(p, entry);
			}
			if (!p)
				TAILQ_INSERT_TAIL(&head, e, entry);
		}
		close(fd);

		printf("Type        "
		       "ClusterId (pfs_clid)                 "
		       "Label on %s\n", av[i]);
		while ((p = TAILQ_FIRST(&head)) != NULL) {
			printf("%s %s\n", p->s, p->name);
			TAILQ_REMOVE(&head, p, entry);
			free(p);
		}
	}

	if (all)
		put_hammer2_mounts(ac, av);

	return (ecode);
}

int
cmd_pfs_getid(const char *sel_path, const char *name, int privateid)
{
	hammer2_ioc_pfs_t pfs;
	int ecode = 0;
	int fd;
	char *pfs_id_str = NULL;

	if ((fd = hammer2_ioctl_handle(sel_path)) < 0)
		return(1);
	bzero(&pfs, sizeof(pfs));

	snprintf(pfs.name, sizeof(pfs.name), "%s", name);
	if (ioctl(fd, HAMMER2IOC_PFS_LOOKUP, &pfs) < 0) {
		perror("ioctl");
		ecode = 1;
	} else {
		if (privateid)
			hammer2_uuid_to_str(&pfs.pfs_fsid, &pfs_id_str);
		else
			hammer2_uuid_to_str(&pfs.pfs_clid, &pfs_id_str);
		printf("%s\n", pfs_id_str);
		free(pfs_id_str);
		pfs_id_str = NULL;
	}
	close(fd);
	return (ecode);
}

int
cmd_pfs_create(const char *sel_path, const char *name,
	       uint8_t pfs_type, const char *uuid_str)
{
	hammer2_ioc_pfs_t pfs;
	int ecode = 0;
	int fd;

	if (pfs_type == HAMMER2_PFSTYPE_NONE) {
		if (uuid_str)
			pfs_type = HAMMER2_PFSTYPE_SLAVE;
		else
			pfs_type = HAMMER2_PFSTYPE_MASTER;
	}

	if ((fd = hammer2_ioctl_handle(sel_path)) < 0)
		return(1);
	bzero(&pfs, sizeof(pfs));
	snprintf(pfs.name, sizeof(pfs.name), "%s", name);
	pfs.pfs_type = pfs_type;
	if (uuid_str) {
		hammer2_uuid_from_string(uuid_str, &pfs.pfs_clid);
	} else {
		hammer2_uuid_create(&pfs.pfs_clid);
	}
	if (1  )
		hammer2_uuid_create(&pfs.pfs_fsid);
	if (1  ) {
		if (ioctl(fd, HAMMER2IOC_PFS_CREATE, &pfs) < 0) {
			if (errno == EEXIST) {
				fprintf(stderr,
					"NOTE: Typically the same name is "
					"used for cluster elements on "
					"different mounts,\n"
					"      but cluster elements on the "
					"same mount require unique names.\n"
					"hammer2: pfs_create(%s): already present\n",
					name);
			} else {
				fprintf(stderr, "hammer2: pfs_create(%s): %s\n",
				       name, strerror(errno));
			}
			ecode = 1;
		} else {
			printf("hammer2: pfs_create(%s): SUCCESS\n", name);
		}
	} else {
		fprintf(stderr, "hammer2: pfs_create(%s): badly formed uuid\n",
			name);
		ecode = 1;
	}
	close(fd);
	return (ecode);
}

int
cmd_pfs_delete(const char *sel_path, char **av, int ac)
{
	hammer2_ioc_pfs_t pfs;
	int ecode = 0;
	int fd;
	int i;
	int n;
	int use_fd;
	int nmnts = 0;
	char **mnts = NULL;

	if (sel_path == NULL)
		mnts = get_hammer2_mounts(&nmnts);

	for (i = 1; i < ac; ++i) {
		int enoents = 0;
		bzero(&pfs, sizeof(pfs));
		snprintf(pfs.name, sizeof(pfs.name), "%s", av[i]);

		if (sel_path) {
			use_fd = hammer2_ioctl_handle(sel_path);
		} else {
			use_fd = -1;
			for (n = 0; n < nmnts; ++n) {
				if ((fd = hammer2_ioctl_handle(mnts[n])) < 0) {
					enoents++;
					continue;
				}
				if (ioctl(fd, HAMMER2IOC_PFS_LOOKUP, &pfs) < 0) {
					enoents++;
					continue;
				}
				if (use_fd >= 0) {
					fprintf(stderr,
						"hammer2: pfs_delete(%s): "
						"Duplicate PFS name, "
						"must specify mount\n",
						av[i]);
					close(use_fd);
					use_fd = -1;
					break;
				}
				use_fd = fd;
			}
		}
		if (use_fd >= 0) {
			if (ioctl(use_fd, HAMMER2IOC_PFS_DELETE, &pfs) < 0) {
				printf("hammer2: pfs_delete(%s): %s\n",
				       av[i], strerror(errno));
				ecode = 1;
			} else {
				printf("hammer2: pfs_delete(%s): SUCCESS\n",
				       av[i]);
			}
			close(use_fd);
		} else {
			if (enoents == nmnts)
				printf("hammer2: pfs_delete(%s): %s not found\n",
				       av[i], av[i]);
			else
				printf("hammer2: pfs_delete(%s): FAILED\n",
				       av[i]);
			ecode = 1;
		}
	}
	if (mnts)
		put_hammer2_mounts(nmnts, mnts);
	return (ecode);
}
