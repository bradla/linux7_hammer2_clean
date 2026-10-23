#include <sys/types.h>
#include <sys/stat.h>
#include <netdb.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>
#include <sys/dmsg.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <sys/dfly.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <libutil.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <time.h>

#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>
#include <openssl/evp.h>

#include <machine/atomic.h>
#include <byteswap.h>

#include "dmsg.h"

uint32_t iscsi_crc32(const void *buf, size_t size);
uint32_t iscsi_crc32_ext(const void *buf, size_t size, uint32_t ocrc);
