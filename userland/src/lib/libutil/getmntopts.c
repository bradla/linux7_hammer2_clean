#include <sys/param.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <mntopts.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>

int getmnt_silent = 0;

void
getmntopts(const char *options, const struct mntopt *m0, int *flagp,
           int *altflagp)
{
	const struct mntopt *m;
	int negative;
	char *opt, *optbuf, *p;
	int *thisflagp;

	if ((optbuf = strdup(options)) == NULL)
		err(1, NULL);

	for (opt = optbuf; (opt = strtok(opt, ",")) != NULL; opt = NULL) {

		if (opt[0] == 'n' && opt[1] == 'o') {
			negative = 1;
			opt += 2;
		} else
			negative = 0;

		p = strchr(opt, '=');
		if (p)
			 *++p = '\0';

		for (m = m0; m->m_option != NULL; ++m) {
			if (strcasecmp(opt, m->m_option) == 0)
				break;
		}

		if (m->m_option) {
			thisflagp = m->m_altloc ? altflagp : flagp;
			if (negative == m->m_inverse)
				*thisflagp |= m->m_flag;
			else
				*thisflagp &= ~m->m_flag;
		} else if (!getmnt_silent) {
			errx(1, "-o %s: option not supported", opt);
		}
	}

	free(optbuf);
}

void
rmslashes(char *rrpin, char *rrpout)
{
	char *rrpoutstart;

	*rrpout = *rrpin;
	for (rrpoutstart = rrpout; *rrpin != '\0'; *rrpout++ = *rrpin++) {

		while (*rrpin == '/' && *(rrpin + 1) == '/')
			 rrpin++;
	}

	if (rrpout - rrpoutstart > 1 && *(rrpout - 1) == '/')
		*(rrpout - 1) = '\0';
	else
		*rrpout = '\0';
}

void
checkpath(const char *path, char *resolved)
{
	struct stat sb;

	if (realpath(path, resolved) != NULL && stat(resolved, &sb) == 0) {
		if (!S_ISDIR(sb.st_mode))
			errx(EX_USAGE, "%s: not a directory", resolved);
	} else
		errx(EX_USAGE, "%s: %s", resolved, strerror(errno));
}
