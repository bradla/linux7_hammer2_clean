#include <sys/types.h>
#include <sys/param.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

struct old_ps_strings {
	char	*old_ps_argvstr;
	int	old_ps_nargvstr;
	char	*old_ps_envstr;
	int	old_ps_nenvstr;
};
#define	OLD_PS_STRINGS ((struct old_ps_strings *) \
	(USRSTACK - SPARE_USRSPACE - sizeof(struct old_ps_strings)))

#include <stdarg.h>

#define SPT_BUFSIZE 2048

void
setproctitle(const char *fmt, ...)
{
#if 0
	static struct ps_strings *ps_strings;
	static char *buf = NULL;
	static char *obuf = NULL;
	static char **oargv, *kbuf;
	static int oargc = -1;
	static char *nargv[2] = { NULL, NULL };
	char **nargvp;
	int nargc;
	int i;
	va_list ap;
	size_t len;
	unsigned long ul_ps_strings;
	int oid[4];

	va_start(ap, fmt);

	if (__ukp_spt(fmt, ap) == 0) {
		va_end(ap);
		return;
	}

	if (buf == NULL) {
		buf = malloc(SPT_BUFSIZE);
		if (buf == NULL) {
			va_end(ap);
			return;
		}
		nargv[0] = buf;
	}

	if (obuf == NULL ) {
		obuf = malloc(SPT_BUFSIZE);
		if (obuf == NULL) {
			va_end(ap);
			return;
		}
		*obuf = '\0';
	}

	if (fmt) {
		buf[SPT_BUFSIZE - 1] = '\0';

		if (fmt[0] == '-') {

			fmt++;
			len = 0;
		} else {

			snprintf(buf, SPT_BUFSIZE, "%s: ", _getprogname());
			len = strlen(buf);
		}

		vsnprintf(buf + len, SPT_BUFSIZE - len, fmt, ap);

		nargvp = nargv;
		nargc = 1;
		kbuf = buf;
	} else if (*obuf != '\0') {

		nargvp = oargv;
		nargc = oargc;
		kbuf = obuf;
	} else {

		va_end(ap);
		return;
	}

	va_end(ap);

	oid[0] = CTL_KERN;
	oid[1] = KERN_PROC;
	oid[2] = KERN_PROC_ARGS;
	oid[3] = getpid();
	sysctl(oid, 4, 0, 0, kbuf, strlen(kbuf) + 1);

	if (ps_strings == NULL) {
		len = sizeof(ul_ps_strings);
		if (sysctlbyname("kern.ps_strings", &ul_ps_strings, &len, NULL,
		    0) == -1)
			ul_ps_strings = PS_STRINGS;
		ps_strings = (struct ps_strings *)ul_ps_strings;
	}

	if (ps_strings->ps_argvstr) {

		if (oargc == -1) {

			oargc = ps_strings->ps_nargvstr;
			oargv = ps_strings->ps_argvstr;
			for (i = len = 0; i < oargc; i++) {

				if (oargv[i] == NULL) {
					oargc = i;
					break;
				}
				snprintf(obuf + len, SPT_BUFSIZE - len, "%s%s",
				    len ? " " : "", oargv[i]);
				if (len)
					len++;
				len += strlen(oargv[i]);
				if (len >= SPT_BUFSIZE)
					break;
			}
		}
		ps_strings->ps_nargvstr = nargc;
		ps_strings->ps_argvstr = nargvp;
	} else {

		if (*obuf == '\0')
			strncpy(obuf, OLD_PS_STRINGS->old_ps_argvstr,
			    SPT_BUFSIZE - 1);
		OLD_PS_STRINGS->old_ps_nargvstr = 1;
		OLD_PS_STRINGS->old_ps_argvstr = nargvp[0];
	}
#endif
}
