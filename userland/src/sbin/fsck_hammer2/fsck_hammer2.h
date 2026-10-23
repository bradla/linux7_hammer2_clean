#ifndef FSCK_HAMMER2_H_
#define FSCK_HAMMER2_H_

extern int DebugOpt;
extern int ForceOpt;
extern int VerboseOpt;
extern int QuietOpt;
extern int CountEmpty;
extern int ScanBest;
extern int ScanPFS;
extern int PrintPFS;
extern int NumPFSNames;
extern char **PFSNames;
extern long BlockrefCacheCount;

int test_hammer2(const char *);

#endif
