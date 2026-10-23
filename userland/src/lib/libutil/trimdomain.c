#include <sys/param.h>

#include <libutil.h>
#include <string.h>
#include <unistd.h>

static int	isDISP(const char *);

void
trimdomain(char *fullhost, int hostsize)
{
	static size_t dlen;
	static int first = 1;
	static char domain[MAXHOSTNAMELEN];
	char *end, *s;
	size_t len;

	if (first) {

		first = 0;
		if (gethostname(domain, sizeof(domain) - 1) == 0 &&
		    (s = strchr(domain, '.')) != NULL)
			memmove(domain, s + 1, strlen(s + 1) + 1);
		else
			domain[0] = '\0';
		dlen = strlen(domain);
	}

	if (domain[0] == '\0')
		return;

	s = fullhost;
	end = s + hostsize + 1;
	if ((s = memchr(s, '.', (size_t)(end - s))) != NULL) {
		if (strncasecmp(s + 1, domain, dlen) == 0) {
			if (s[dlen + 1] == '\0') {

				*s = '\0';
			} else if (s[dlen + 1] == ':' &&
			    isDISP(s + dlen + 2) &&
			    (len = strlen(s + dlen + 1)) < (size_t)(end - s)) {

				memmove(s, s + dlen + 1, len + 1);
			}
		}
	}
}

static int
isDISP(const char *disp)
{
	size_t w;
	int res;

	w = strspn(disp, "0123456789");
	res = 0;
	if (w > 0) {
		if (disp[w] == '\0')
			res = 1;
		else if (disp[w] == '.') {
			disp += w + 1;
			w = strspn(disp, "0123456789");
			if (w > 0 && disp[w] == '\0')
				res = 1;
		}
	}
	return (res);
}
