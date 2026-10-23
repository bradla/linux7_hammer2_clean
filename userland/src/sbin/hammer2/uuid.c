#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <byteswap.h>
#include <uuid/uuid.h>

#include "hammer2_subs.h"

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
	p->time_low = bswap_32(p->time_low);
	p->time_mid = bswap_16(p->time_mid);
	p->time_hi_and_version = bswap_16(p->time_hi_and_version);
}

void hammer2_uuid_create(hammer2_uuid_t *uuid)
{
	uuid_generate(uuid->uuid);
}

int hammer2_uuid_from_string(const char *str, hammer2_uuid_t *uuid)
{
	if (uuid_parse(str, uuid->uuid))
		return(-1);

	__uuid_swap((struct __uuid*)&uuid->uuid);

	return(0);
}

int hammer2_uuid_to_string(const hammer2_uuid_t *uuid, char **str)
{
	struct __uuid u;

	memcpy(&u, uuid->uuid, sizeof(u));
	__uuid_swap(&u);

	*str = calloc(64, sizeof(char*));
	uuid_unparse((void*)&u, *str);

	return(0);
}

int hammer2_uuid_name_lookup(hammer2_uuid_t *uuid, const char *str)
{
	if (strcmp(str, "DragonFly HAMMER2") ||
	    hammer2_uuid_from_string(HAMMER2_UUID_STRING, uuid)) {
		memset(uuid, 0, sizeof(*uuid));
		return(-1);
	}

	return(0);
}

int hammer2_uuid_addr_lookup(const hammer2_uuid_t *uuid, char **str)
{
	hammer2_uuid_t tmp;

	if (hammer2_uuid_from_string(HAMMER2_UUID_STRING, &tmp) ||
	    memcmp(uuid, &tmp, sizeof(tmp))) {
		*str = NULL;
		return(-1);
	}
	*str = strdup("DragonFly HAMMER2");

	return(0);
}

int hammer2_uuid_compare(const hammer2_uuid_t *uuid1, const hammer2_uuid_t *uuid2)
{
	return(uuid_compare(uuid1->uuid, uuid2->uuid));
}
