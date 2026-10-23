#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#include "fsck_hammer2.h"

int DebugOpt;
int ForceOpt;
int VerboseOpt;
int QuietOpt;
int CountEmpty;
int ScanBest;
int ScanPFS;
int PrintPFS;
int NumPFSNames;
char **PFSNames;
long BlockrefCacheCount = -1;

static void
init_pfs_names(const char *names)
{
	char *name, *h, *p;
	int siz = 32;

	PFSNames = calloc(siz, sizeof(char *));
	p = strdup(names);
	h = p;

	while ((name = p) != NULL) {
		p = strchr(p, ',');
		if (p)
			*p++ = 0;
		if (strlen(name)) {
			if (NumPFSNames > siz - 1) {
				siz *= 2;
				PFSNames = realloc(PFSNames,
				    siz * sizeof(char *));
			}
			PFSNames[NumPFSNames++] = strdup(name);
		}
	}
	free(h);

	if (DebugOpt) {
		int i;
		for (i = 0; i < NumPFSNames; i++)
			printf("PFSNames[%d]=\"%s\"\n", i, PFSNames[i]);
	}
}

static void
cleanup_pfs_names(void)
{
	int i;

	for (i = 0; i < NumPFSNames; i++)
		free(PFSNames[i]);
	free(PFSNames);
}

static void
usage(void)
{
	fprintf(stderr, "fsck_hammer2 [-f] [-v] [-q] [-e] [-b] [-p] [-P] "
	    "[-l pfs_names] [-c cache_count] special\n");
	exit(1);
}

int
main(int ac, char **av)
{
	int i, ch;

	while ((ch = getopt(ac, av, "dfvqebpPl:c:")) != -1) {
		switch(ch) {
		case 'd':
			DebugOpt++;
			break;
		case 'f':
			ForceOpt = 1;
			break;
		case 'v':
			if (QuietOpt)
				--QuietOpt;
			else
				++VerboseOpt;
			break;
		case 'q':
			if (VerboseOpt)
				--VerboseOpt;
			else
				++QuietOpt;
			break;
		case 'e':
			CountEmpty = 1;
			break;
		case 'b':
			ScanBest = 1;
			break;
		case 'p':
			ScanPFS = 1;
			break;
		case 'P':
			PrintPFS = 1;
			break;
		case 'l':
			init_pfs_names(optarg);
			break;
		case 'c':
			errno = 0;
			BlockrefCacheCount = strtol(optarg, NULL, 10);
			if (errno == ERANGE &&
			    (BlockrefCacheCount == LONG_MIN ||
			     BlockrefCacheCount == LONG_MAX)) {
				perror("strtol");
				exit(1);
			}
			break;
		default:
			usage();

			break;
		}
	}

	ac -= optind;
	av += optind;
	if (ac < 1) {
		usage();

	}

	for (i = 0; i < ac; i++) {
		if (ac != 1)
			printf("%s\n", av[i]);
		if (test_hammer2(av[i]) == -1)
			exit(1);
		if (i != ac - 1)
			printf("----------------------------------------"
			       "----------------------------------------\n");
	}

	cleanup_pfs_names();

	return 0;
}
