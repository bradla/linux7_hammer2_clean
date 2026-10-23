#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <byteswap.h>
#include <uuid/uuid.h>

#include "dmsg.h"

struct __uuid {
	uint32_t	time_low;
	uint16_t	time_mid;
	uint16_t	time_hi_and_version;
	uint8_t		clock_seq_hi_and_reserved;
	uint8_t		clock_seq_low;
	uint8_t		node[6];
};

static
void __uuid_swap(struct __uuid *p)
{
	p->time_low = bswap32(p->time_low);
	p->time_mid = bswap16(p->time_mid);
	p->time_hi_and_version = bswap16(p->time_hi_and_version);
}

void dmsg_uuid_create(dmsg_uuid_t *uuid)
{
	uuid_generate(uuid->uuid);
}

int dmsg_uuid_from_string(const char *str, dmsg_uuid_t *uuid)
{
	if (uuid_parse(str, uuid->uuid))
		return(-1);

	__uuid_swap((struct __uuid*)&uuid->uuid);

	return(0);
}

int dmsg_uuid_to_string(const dmsg_uuid_t *uuid, char **str)
{
	struct __uuid u;

	memcpy(&u, uuid->uuid, sizeof(u));
	__uuid_swap(&u);

	*str = calloc(64, sizeof(char*));
	uuid_unparse((void*)&u, *str);

	return(0);
}

int dmsg_uuid_compare(const dmsg_uuid_t *uuid1, const dmsg_uuid_t *uuid2)
{
	return(uuid_compare(uuid1->uuid, uuid2->uuid));
}

int dmsg_uuid_is_nil(dmsg_uuid_t *uuid)
{
	return uuid_is_null(uuid->uuid);
}
