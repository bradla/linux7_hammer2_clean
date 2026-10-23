#ifndef _MNTOPTS_H_
#define _MNTOPTS_H_

#include <sys/cdefs.h>

struct mntopt {
	const char *m_option;
	int m_inverse;
	int m_flag;
	int m_altloc;
};

#define MOPT_ASYNC		{ "async",	0, MNT_ASYNC, 0 }
#define MOPT_NOATIME		{ "atime",	1, MNT_NOATIME, 0 }
#define MOPT_NODEV		{ "dev",	1, MNT_NODEV, 0 }
#define MOPT_NOEXEC		{ "exec",	1, MNT_NOEXEC, 0 }
#define MOPT_NOSUID		{ "suid",	1, MNT_NOSUID, 0 }
#define MOPT_NOSYMFOLLOW	{ "symfollow",  1, MNT_NOSYMFOLLOW, 0 }
#define MOPT_TRIM		{ "trim",	0, MNT_TRIM, 0 }
#define MOPT_RDONLY		{ "rdonly",	0, MNT_RDONLY, 0 }
#define MOPT_SYNC		{ "sync",	0, MNT_SYNCHRONOUS, 0 }
#define MOPT_USERQUOTA		{ "userquota",	0, 0, 0 }
#define MOPT_GROUPQUOTA		{ "groupquota",	0, 0, 0 }
#define MOPT_NOCLUSTERR		{ "clusterr",	1, MNT_NOCLUSTERR, 0 }
#define MOPT_NOCLUSTERW		{ "clusterw",	1, MNT_NOCLUSTERW, 0 }
#define MOPT_SUIDDIR		{ "suiddir",	0, MNT_SUIDDIR, 0 }
#define MOPT_AUTOMOUNTED	{ "automounted",0, MNT_AUTOMOUNTED, 0 }
#define MOPT_IGNORE		{ "ignore",	0, MNT_IGNORE, 0 }

#define MOPT_FORCE		{ "force",	0, MNT_FORCE, 0 }
#define MOPT_UPDATE		{ "update",	0, MNT_UPDATE, 0 }
#define MOPT_RO			{ "ro",		0, MNT_RDONLY, 0 }
#define MOPT_RW			{ "rw",		1, MNT_RDONLY, 0 }

#define MOPT_NULL		{ NULL,         0, 0, 0 }

#define MOPT_AUTO		{ "auto",	0, 0, 0 }

#define MOPT_FSTAB_COMPAT						\
	MOPT_RO,							\
	MOPT_RW,							\
	MOPT_AUTO

#define MOPT_STDOPTS							\
	MOPT_USERQUOTA,							\
	MOPT_GROUPQUOTA,						\
	MOPT_FSTAB_COMPAT,						\
	MOPT_NOATIME,							\
	MOPT_NODEV,							\
	MOPT_NOEXEC,							\
	MOPT_SUIDDIR,		 	\
	MOPT_NOSUID,							\
	MOPT_NOSYMFOLLOW,						\
	MOPT_RDONLY,							\
	MOPT_NOCLUSTERR,						\
	MOPT_NOCLUSTERW,						\
	MOPT_AUTOMOUNTED,						\
	MOPT_IGNORE

extern int getmnt_silent;

__BEGIN_DECLS
void getmntopts(const char *, const struct mntopt *, int *, int *);
void rmslashes(char *, char *);
void checkpath(const char *, char resolved_path[]);
__END_DECLS

#endif
