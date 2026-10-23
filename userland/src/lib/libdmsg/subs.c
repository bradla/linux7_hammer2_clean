#include "dmsg_local.h"

void *
dmsg_alloc(size_t bytes)
{
	void *ptr;

	ptr = malloc(bytes);
	assert(ptr);
	bzero(ptr, bytes);
	return (ptr);
}

void
dmsg_free(void *ptr)
{
	free(ptr);
}

const char *
dmsg_uuid_to_str(dmsg_uuid_t *uuid, char **strp)
{
	if (*strp) {
		free(*strp);
		*strp = NULL;
	}
	dmsg_uuid_to_string(uuid, strp);
	return (*strp);
}

const char *
dmsg_peer_type_to_str(uint8_t type)
{
	switch(type) {
	case DMSG_PEER_NONE:
		return("NONE");
	case DMSG_PEER_ROUTER:
		return("ROUTER");
	case DMSG_PEER_BLOCK:
		return("BLOCK");
	case DMSG_PEER_HAMMER2:
		return("HAMMER2");
	case DMSG_PEER_CLIENT:
		return("CLIENT");
	default:
		return("?PEERTYPE?");
	}
}

int
dmsg_connect(const char *hostname)
{
	struct sockaddr_in lsin;
	struct hostent *hen;
	int fd;
	int opt;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		dm_printf(1, "cmd_debug: socket(): %s\n",
			  strerror(errno));
		return -1;
	}
	opt = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof opt);

	bzero(&lsin, sizeof(lsin));
	lsin.sin_family = AF_INET;
	lsin.sin_addr.s_addr = 0;
	lsin.sin_port = htons(DMSG_LISTEN_PORT);

	if (hostname) {
		hen = gethostbyname2(hostname, AF_INET);
		if (hen == NULL) {
			if (inet_pton(AF_INET, hostname, &lsin.sin_addr) != 1) {
				dm_printf(3, "Cannot resolve %s\n", hostname);
				return -1;
			}
		} else {
			bcopy(hen->h_addr, &lsin.sin_addr, hen->h_length);
		}
	}
	if (connect(fd, (struct sockaddr *)&lsin, sizeof(lsin)) < 0) {
		close(fd);
		if (DMsgDebugOpt > 2) {
			dm_printf(3, "debug: Connect failed: %s\n",
				  strerror(errno));
		}
		return -1;
	}
	return (fd);
}
