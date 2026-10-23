#ifndef NEWFS_HAMMER2_H_
#define NEWFS_HAMMER2_H_

#include <vfs/hammer2/hammer2_disk.h>

#include <uuid/uuid.h>

#include "hammer2_subs.h"

#define HAMMER2_LABEL_NONE	0
#define HAMMER2_LABEL_BOOT	1
#define HAMMER2_LABEL_ROOT	2
#define HAMMER2_LABEL_DATA	3

#define MAXLABELS	HAMMER2_SET_COUNT

typedef struct {
	int Hammer2Version;
	hammer2_uuid_t Hammer2_FSType;
	hammer2_uuid_t Hammer2_VolFSID;
	hammer2_uuid_t Hammer2_SupCLID;
	hammer2_uuid_t Hammer2_SupFSID;
	hammer2_uuid_t Hammer2_PfsCLID[MAXLABELS];
	hammer2_uuid_t Hammer2_PfsFSID[MAXLABELS];
	hammer2_off_t BootAreaSize;
	hammer2_off_t AuxAreaSize;
	hammer2_off_t FileSystemSize[HAMMER2_MAX_VOLUMES];
	int NFileSystemSizes;
	char *Label[MAXLABELS];
	int NLabels;
	int CompType;
	int CheckType;
	int DefaultLabelType;
	int DebugOpt;
} hammer2_mkfs_options_t;

void hammer2_mkfs_init(hammer2_mkfs_options_t *opt);
void hammer2_mkfs_cleanup(hammer2_mkfs_options_t *opt);

int64_t getsize(const char *str, int64_t minval, int64_t maxval, int pw);

void hammer2_mkfs(int ac, char **av, hammer2_mkfs_options_t *opt);

#endif
