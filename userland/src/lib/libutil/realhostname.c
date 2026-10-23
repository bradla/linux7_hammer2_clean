#include <sys/param.h>
#include <sys/socket.h>

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdio.h>
#include <string.h>
#include <libutil.h>

#include "../libc/string/util.h"

struct sockinet {
	u_char	si_len;
	u_char	si_family;
	u_short	si_port;
};

#define SOCKADDR_IN6(p) ((struct sockaddr_in6 *)(void *)(p))
#define SOCKADDR_IN(p) ((struct sockaddr_in *)(void *)(p))
#define SOCKINET(p) ((struct sockinet *)(void *)(p))

int
realhostname_sa(char *host, size_t hsize, struct sockaddr *addr, int addrlen)
{
	int result, error;
	char buf[NI_MAXHOST];
#ifdef INET6
	struct sockaddr_in lsin;
#endif

	result = HOSTNAME_INVALIDADDR;

#ifdef INET6

	if (addr->sa_family == AF_INET6 &&
	    addrlen == sizeof(struct sockaddr_in6) &&
	    IN6_IS_ADDR_V4MAPPED(&SOCKADDR_IN6(addr)->sin6_addr)) {
		struct sockaddr_in6 *sin6;

		sin6 = SOCKADDR_IN6(addr);

		memset(&lsin, 0, sizeof(lsin));
		lsin.sin_len = sizeof(struct sockaddr_in);
		lsin.sin_family = AF_INET;
		lsin.sin_port = sin6->sin6_port;
		memcpy(&lsin.sin_addr, &sin6->sin6_addr.s6_addr[12],
		       sizeof(struct in_addr));
		addr = (struct sockaddr *)&lsin;
		addrlen = lsin.sin_len;
	}
#endif

	error = getnameinfo(addr, addrlen, buf, sizeof(buf), NULL, 0,
			    NI_NAMEREQD);
	if (error == 0) {
		struct addrinfo hints, *res, *ores;
		struct sockaddr *sa;

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = addr->sa_family;
		hints.ai_flags = AI_CANONNAME | AI_PASSIVE;
		hints.ai_socktype = SOCK_STREAM;

		error = getaddrinfo(buf, NULL, &hints, &res);
		if (error) {
			result = HOSTNAME_INVALIDNAME;
			goto numeric;
		}
		for (ores = res; ; res = res->ai_next) {
			if (res == NULL) {
				freeaddrinfo(ores);
				result = HOSTNAME_INCORRECTNAME;
				goto numeric;
			}
			sa = res->ai_addr;
			if (sa == NULL) {
				freeaddrinfo(ores);
				result = HOSTNAME_INCORRECTNAME;
				goto numeric;
			}
			if (
			    sa->sa_family == addr->sa_family) {
				SOCKINET(sa)->si_port = SOCKINET(addr)->si_port;
#ifdef INET6

				if (sa->sa_family == AF_INET6 &&
				    SOCKADDR_IN6(sa)->sin6_scope_id == 0)
					SOCKADDR_IN6(sa)->sin6_scope_id =
					    SOCKADDR_IN6(addr)->sin6_scope_id;
#endif
				if (!memcmp(sa, addr, 4  )) {
					result = HOSTNAME_FOUND;
					if (ores->ai_canonname == NULL) {
						freeaddrinfo(ores);
						goto numeric;
					}
					strlcpy(buf, ores->ai_canonname,
						sizeof(buf));
					trimdomain(buf, hsize);
					if (strlen(buf) > hsize &&
					    addr->sa_family == AF_INET) {
						freeaddrinfo(ores);
						goto numeric;
					}
					strncpy(host, buf, hsize);
					break;
				}
			}
		}
		freeaddrinfo(ores);
	} else {
    numeric:
		if (getnameinfo(addr, addrlen, buf, sizeof(buf), NULL, 0,
				NI_NUMERICHOST) == 0)
			strncpy(host, buf, hsize);
	}

	return result;
}
