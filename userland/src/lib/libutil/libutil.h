#ifndef _LIBUTIL_H_
#define	_LIBUTIL_H_

#include <sys/types.h>
#include <sys/socket.h>
#include <stdint.h>

struct pidfh;

void	trimdomain(char *, int);
int	flopen(const char *_path, int _flags, ...);
void	hexdump(const void *_ptr, int _length, const char *_hdr, int _flags);
int	humanize_unsigned(char *buf, size_t len, uint64_t bytes,
					const char *suffix, int divisor);
int	format_bytes(char *buf, size_t len, uint64_t bytes);
int	pidfile_close(struct pidfh *_pfh);
int	pidfile_fileno(const struct pidfh *_pfh);
struct pidfh *
	pidfile_open(const char *_path, mode_t _mode, pid_t *_pidptr);
int	pidfile_remove(struct pidfh *_pfh);
int	pidfile_write(struct pidfh *_pfh);
int	realhostname_sa(char *, size_t, struct sockaddr *, int);

#define	HD_COLUMN_MASK		0xff
#define	HD_DELIM_MASK		0xff00
#define	HD_OMIT_COUNT		(1 << 16)
#define	HD_OMIT_HEX		(1 << 17)
#define	HD_OMIT_CHARS		(1 << 18)

#define	HOSTNAME_FOUND		0
#define	HOSTNAME_INCORRECTNAME	1
#define	HOSTNAME_INVALIDADDR	2
#define	HOSTNAME_INVALIDNAME	3

#endif
