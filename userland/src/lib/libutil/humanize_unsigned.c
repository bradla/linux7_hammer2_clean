#include <stdio.h>
#include <string.h>
#include <libutil.h>

int
humanize_unsigned(char *buf, size_t len, uint64_t bytes, const char *suffix,
    int divisor)
{

	const char *prefixes;
	int		r;
	uint64_t	umax;
	size_t		i, suffixlen;

	if (buf == NULL)
		return (-1);
	if (len > 0)
		buf[0] = '\0';
	if (suffix == NULL)
		suffix = "";
	suffixlen = strlen(suffix);

	if (len < 4 + suffixlen)
		return (-1);

	if (divisor == 1024) {

		prefixes = " KMGTPE";
	} else
		prefixes = " kMGTPE";

	umax = 1;
	for (i = 0; i < len - suffixlen - 3; i++) {
		umax *= 10;
		if (umax > bytes)
			break;
	}
	for (i = 0; bytes >= umax && prefixes[i + 1]; i++)
		bytes /= divisor;

	r = snprintf(buf, len, "%qu%s%c%s", (unsigned long long)bytes,
	    i == 0 ? "" : " ", prefixes[i], suffix);

	return (r);
}

int
format_bytes(char *buf, size_t len, uint64_t bytes)
{
	int	rv;
	size_t	nlen;

	rv = humanize_unsigned(buf, len, bytes, "B", 1024);
	if (rv != -1) {

		nlen = strlen(buf) - 2;
		if (strcmp(&buf[nlen], " B") == 0)
			buf[nlen] = '\0';
	}
	return (rv);
}
