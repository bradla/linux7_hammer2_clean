#ifndef _FS_HAMMER2_XXHASH_H_
#define _FS_HAMMER2_XXHASH_H_

#include <linux/xxhash.h>
#define XXH64(input, len, seed)	xxh64((input), (len), (seed))

#define XXH_HAMMER2_SEED        0x4d617474446c6c6eLLU

#endif
