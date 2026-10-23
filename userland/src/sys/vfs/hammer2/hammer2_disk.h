#ifndef _VFS_HAMMER2_DISK_H_
#define _VFS_HAMMER2_DISK_H_

#include <stdint.h>
#include <sys/dmsg.h>
#include <sys/dfly.h>
#include <uuid/uuid.h>

#define HAMMER2_ALLOC_MIN	1024
#define HAMMER2_RADIX_MIN	10
#define HAMMER2_ALLOC_MAX	65536
#define HAMMER2_RADIX_MAX	16
#define HAMMER2_RADIX_KEY	64

#define HAMMER2_SEGSIZE		(1 << HAMMER2_FREEMAP_LEVEL0_RADIX)
#define HAMMER2_SEGRADIX	HAMMER2_FREEMAP_LEVEL0_RADIX

#define HAMMER2_PBUFRADIX	16
#define HAMMER2_PBUFSIZE	65536
#define HAMMER2_LBUFRADIX	14
#define HAMMER2_LBUFSIZE	16384

#define HAMMER2_IND_BYTES_MIN	4096
#define HAMMER2_IND_BYTES_NOM	HAMMER2_LBUFSIZE
#define HAMMER2_IND_BYTES_MAX	HAMMER2_PBUFSIZE
#define HAMMER2_IND_RADIX_MIN	12
#define HAMMER2_IND_RADIX_NOM	HAMMER2_LBUFRADIX
#define HAMMER2_IND_RADIX_MAX	HAMMER2_PBUFRADIX
#define HAMMER2_IND_COUNT_MIN	(HAMMER2_IND_BYTES_MIN / \
				 sizeof(hammer2_blockref_t))
#define HAMMER2_IND_COUNT_NOM	(HAMMER2_IND_BYTES_NOM / \
				 sizeof(hammer2_blockref_t))
#define HAMMER2_IND_COUNT_MAX	(HAMMER2_IND_BYTES_MAX / \
				 sizeof(hammer2_blockref_t))

#define HAMMER2_SET_RADIX		2
#define HAMMER2_SET_COUNT		(1 << HAMMER2_SET_RADIX)
#define HAMMER2_EMBEDDED_BYTES		512
#define HAMMER2_EMBEDDED_RADIX		9

#define HAMMER2_PBUFMASK	(HAMMER2_PBUFSIZE - 1)
#define HAMMER2_LBUFMASK	(HAMMER2_LBUFSIZE - 1)
#define HAMMER2_SEGMASK		(HAMMER2_SEGSIZE - 1)

#define HAMMER2_LBUFMASK64	((hammer2_off_t)HAMMER2_LBUFMASK)
#define HAMMER2_PBUFSIZE64	((hammer2_off_t)HAMMER2_PBUFSIZE)
#define HAMMER2_PBUFMASK64	((hammer2_off_t)HAMMER2_PBUFMASK)
#define HAMMER2_SEGSIZE64	((hammer2_off_t)HAMMER2_SEGSIZE)
#define HAMMER2_SEGMASK64	((hammer2_off_t)HAMMER2_SEGMASK)

#define HAMMER2_UUID_STRING	"5cbb9ad1-862d-11dc-a94d-01301bb8a9f5"

#define HAMMER2_VOLUME_ALIGN		(8 * 1024 * 1024)
#define HAMMER2_VOLUME_ALIGN64		((hammer2_off_t)HAMMER2_VOLUME_ALIGN)
#define HAMMER2_VOLUME_ALIGNMASK	(HAMMER2_VOLUME_ALIGN - 1)
#define HAMMER2_VOLUME_ALIGNMASK64	((hammer2_off_t)HAMMER2_VOLUME_ALIGNMASK)

#define HAMMER2_NEWFS_ALIGN		(HAMMER2_VOLUME_ALIGN)
#define HAMMER2_NEWFS_ALIGN64		((hammer2_off_t)HAMMER2_VOLUME_ALIGN)
#define HAMMER2_NEWFS_ALIGNMASK		(HAMMER2_VOLUME_ALIGN - 1)
#define HAMMER2_NEWFS_ALIGNMASK64	((hammer2_off_t)HAMMER2_NEWFS_ALIGNMASK)

#define HAMMER2_ZONE_BYTES64		(2LLU * 1024 * 1024 * 1024)
#define HAMMER2_ZONE_MASK64		(HAMMER2_ZONE_BYTES64 - 1)
#define HAMMER2_ZONE_SEG		(4 * 1024 * 1024)
#define HAMMER2_ZONE_SEG64		((hammer2_off_t)HAMMER2_ZONE_SEG)
#define HAMMER2_ZONE_BLOCKS_SEG		(HAMMER2_ZONE_SEG / HAMMER2_PBUFSIZE)

#define HAMMER2_ZONE_FREEMAP_INC	5

#define HAMMER2_ZONE_VOLHDR		0
#define HAMMER2_ZONE_FREEMAP_00		1
#define HAMMER2_ZONE_FREEMAP_01		6
#define HAMMER2_ZONE_FREEMAP_02		11
#define HAMMER2_ZONE_FREEMAP_03		16
#define HAMMER2_ZONE_FREEMAP_04		21
#define HAMMER2_ZONE_FREEMAP_05		26
#define HAMMER2_ZONE_FREEMAP_06		31
#define HAMMER2_ZONE_FREEMAP_07		36
#define HAMMER2_ZONE_FREEMAP_END	41

#define HAMMER2_ZONE_UNUSED41		41
#define HAMMER2_ZONE_UNUSED42		42
#define HAMMER2_ZONE_UNUSED43		43
#define HAMMER2_ZONE_UNUSED44		44
#define HAMMER2_ZONE_UNUSED45		45
#define HAMMER2_ZONE_UNUSED46		46
#define HAMMER2_ZONE_UNUSED47		47
#define HAMMER2_ZONE_UNUSED48		48
#define HAMMER2_ZONE_UNUSED49		49
#define HAMMER2_ZONE_UNUSED50		50
#define HAMMER2_ZONE_UNUSED51		51
#define HAMMER2_ZONE_UNUSED52		52
#define HAMMER2_ZONE_UNUSED53		53
#define HAMMER2_ZONE_UNUSED54		54
#define HAMMER2_ZONE_UNUSED55		55
#define HAMMER2_ZONE_UNUSED56		56
#define HAMMER2_ZONE_UNUSED57		57
#define HAMMER2_ZONE_UNUSED58		58
#define HAMMER2_ZONE_UNUSED59		59
#define HAMMER2_ZONE_UNUSED60		60
#define HAMMER2_ZONE_UNUSED61		61
#define HAMMER2_ZONE_UNUSED62		62
#define HAMMER2_ZONE_UNUSED63		63
#define HAMMER2_ZONE_END		64

#define HAMMER2_NFREEMAPS		8

#define HAMMER2_ZONEFM_LEVEL1		0
#define HAMMER2_ZONEFM_LEVEL2		1
#define HAMMER2_ZONEFM_LEVEL3		2
#define HAMMER2_ZONEFM_LEVEL4		3
#define HAMMER2_ZONEFM_LEVEL5		4

#if HAMMER2_SET_COUNT != 4
#error "hammer2_disk.h - freemap assumes SET_COUNT is 4"
#endif
#define HAMMER2_FREEMAP_LEVEL6_RADIX	64
#define HAMMER2_FREEMAP_LEVEL5_RADIX	62
#define HAMMER2_FREEMAP_LEVEL4_RADIX	54
#define HAMMER2_FREEMAP_LEVEL3_RADIX	46
#define HAMMER2_FREEMAP_LEVEL2_RADIX	38
#define HAMMER2_FREEMAP_LEVEL1_RADIX	30
#define HAMMER2_FREEMAP_LEVEL0_RADIX	22

#define HAMMER2_FREEMAP_LEVELN_PSIZE	32768

#define HAMMER2_FREEMAP_LEVEL5_SIZE	((hammer2_off_t)1 <<		\
					 HAMMER2_FREEMAP_LEVEL5_RADIX)
#define HAMMER2_FREEMAP_LEVEL4_SIZE	((hammer2_off_t)1 <<		\
					 HAMMER2_FREEMAP_LEVEL4_RADIX)
#define HAMMER2_FREEMAP_LEVEL3_SIZE	((hammer2_off_t)1 <<		\
					 HAMMER2_FREEMAP_LEVEL3_RADIX)
#define HAMMER2_FREEMAP_LEVEL2_SIZE	((hammer2_off_t)1 <<		\
					 HAMMER2_FREEMAP_LEVEL2_RADIX)
#define HAMMER2_FREEMAP_LEVEL1_SIZE	((hammer2_off_t)1 <<		\
					 HAMMER2_FREEMAP_LEVEL1_RADIX)
#define HAMMER2_FREEMAP_LEVEL0_SIZE	((hammer2_off_t)1 <<		\
					 HAMMER2_FREEMAP_LEVEL0_RADIX)

#define HAMMER2_FREEMAP_LEVEL5_MASK	(HAMMER2_FREEMAP_LEVEL5_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL4_MASK	(HAMMER2_FREEMAP_LEVEL4_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL3_MASK	(HAMMER2_FREEMAP_LEVEL3_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL2_MASK	(HAMMER2_FREEMAP_LEVEL2_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL1_MASK	(HAMMER2_FREEMAP_LEVEL1_SIZE - 1)
#define HAMMER2_FREEMAP_LEVEL0_MASK	(HAMMER2_FREEMAP_LEVEL0_SIZE - 1)

#define HAMMER2_FREEMAP_COUNT		(int)(HAMMER2_FREEMAP_LEVELN_PSIZE / \
					 sizeof(hammer2_bmap_data_t))

#define H2FMZONEBASE(key)	((key) & ~HAMMER2_FREEMAP_LEVEL1_MASK)
#define H2FMBASE(key, radix)	rounddown2(key, (hammer2_off_t)1 << (radix))

#define HAMMER2_FREEMAP_BLOCK_RADIX	14
#define HAMMER2_FREEMAP_BLOCK_SIZE	(1 << HAMMER2_FREEMAP_BLOCK_RADIX)
#define HAMMER2_FREEMAP_BLOCK_MASK	(HAMMER2_FREEMAP_BLOCK_SIZE - 1)

typedef uint64_t hammer2_bitmap_t;

#define HAMMER2_BMAP_ALLONES		((hammer2_bitmap_t)-1)
#define HAMMER2_BMAP_ELEMENTS		8
#define HAMMER2_BMAP_BITS_PER_ELEMENT	64
#define HAMMER2_BMAP_INDEX_RADIX	5
#define HAMMER2_BMAP_BLOCKS_PER_ELEMENT	(1 << HAMMER2_BMAP_INDEX_RADIX)

#define HAMMER2_BMAP_INDEX_SIZE		(HAMMER2_FREEMAP_BLOCK_SIZE * \
					 HAMMER2_BMAP_BLOCKS_PER_ELEMENT)
#define HAMMER2_BMAP_INDEX_MASK		(HAMMER2_BMAP_INDEX_SIZE - 1)

#define HAMMER2_BMAP_SIZE		(HAMMER2_BMAP_INDEX_SIZE * \
					 HAMMER2_BMAP_ELEMENTS)
#define HAMMER2_BMAP_MASK		(HAMMER2_BMAP_SIZE - 1)

#define HAMMER2_BOOT_MIN_BYTES		HAMMER2_VOLUME_ALIGN
#define HAMMER2_BOOT_NOM_BYTES		(64*1024*1024)
#define HAMMER2_BOOT_MAX_BYTES		(256*1024*1024)

#define HAMMER2_AUX_MIN_BYTES		HAMMER2_VOLUME_ALIGN
#define HAMMER2_AUX_NOM_BYTES		(256*1024*1024)
#define HAMMER2_AUX_MAX_BYTES		(1024*1024*1024)

typedef uint64_t hammer2_tid_t;
typedef uint64_t hammer2_off_t;
typedef uint64_t hammer2_key_t;
typedef uint32_t hammer2_crc32_t;

typedef struct hammer2_uuid {
	uuid_t uuid;
} hammer2_uuid_t;

#define HAMMER2_TID_MIN		1ULL
#define HAMMER2_TID_MAX		0xFFFFFFFFFFFFFFFFULL
#define HAMMER2_KEY_MIN		0ULL
#define HAMMER2_KEY_MAX		0xFFFFFFFFFFFFFFFFULL

#define HAMMER2_OFF_MASK	0xFFFFFFFFFFFFFFC0ULL
#define HAMMER2_OFF_MASK_LO	(HAMMER2_OFF_MASK & HAMMER2_PBUFMASK64)
#define HAMMER2_OFF_MASK_HI	(~HAMMER2_PBUFMASK64)
#define HAMMER2_OFF_MASK_RADIX	0x000000000000003FULL

#define HAMMER2_DIRHASH_VISIBLE	0x8000000000000000ULL
#define HAMMER2_DIRHASH_USERMSK	0x7FFFFFFFFFFFFFFFULL
#define HAMMER2_DIRHASH_LOMASK	0x0000000000007FFFULL
#if 0
#define HAMMER2_DIRHASH_HIMASK	0xFFFFFFFFFFFF0000ULL
#define HAMMER2_DIRHASH_FORCED	0x0000000000008000ULL
#endif

#define HAMMER2_SROOT_KEY	0x0000000000000000ULL
#define HAMMER2_BOOT_KEY	0xd9b36ce135528000ULL

struct hammer2_volconf {
	uint8_t	copyid;
	uint8_t inprog;
	uint8_t chain_to;
	uint8_t chain_from;
	uint16_t flags;
	uint8_t error;
	uint8_t priority;
	uint8_t remote_pfs_type;
	uint8_t reserved08[23];
	hammer2_uuid_t pfs_clid;
	uint8_t label[16];
	uint8_t path[64];
} __packed;

typedef struct hammer2_volconf hammer2_volconf_t;

#define DMSG_VOLF_ENABLED	0x0001
#define DMSG_VOLF_INPROG	0x0002
#define DMSG_VOLF_CONN_RR	0x80
#define DMSG_VOLF_CONN_EF	0x40
#define DMSG_VOLF_CONN_PRI	0x0F

struct dmsg_lnk_hammer2_volconf {
	dmsg_hdr_t		head;
	hammer2_volconf_t	copy;
	int32_t			index;
	int32_t			unused01;
	hammer2_uuid_t		mediaid;
	int64_t			reserved02[32];
} __packed;

typedef struct dmsg_lnk_hammer2_volconf dmsg_lnk_hammer2_volconf_t;

#define DMSG_LNK_HAMMER2_VOLCONF DMSG_LNK(DMSG_LNK_CMD_HAMMER2_VOLCONF, \
					  dmsg_lnk_hammer2_volconf)

#define H2_LNK_VOLCONF(msg)	((dmsg_lnk_hammer2_volconf_t *)(msg)->any.buf)

struct hammer2_dirent_head {
	hammer2_tid_t		inum;
	uint16_t		namlen;
	uint8_t			type;
	uint8_t			unused0B;
	uint8_t			unused0C[4];
} __packed;

typedef struct hammer2_dirent_head hammer2_dirent_head_t;

struct hammer2_blockref {
	uint8_t		type;
	uint8_t		methods;
	uint8_t		copyid;
	uint8_t		keybits;
	uint8_t		vradix;
	uint8_t		flags;
	uint16_t	leaf_count;
	hammer2_key_t	key;
	hammer2_tid_t	mirror_tid;
	hammer2_tid_t	modify_tid;
	hammer2_off_t	data_off;
	hammer2_tid_t	update_tid;
	union {
		char	buf[16];

		hammer2_dirent_head_t dirent;

		struct {
			hammer2_key_t	data_count;
			hammer2_key_t	inode_count;
		} stats;
	} embed;
	union {
		char	buf[64];
		struct {
			uint32_t value;
			uint32_t reserved[15];
		} iscsi32;
		struct {
			uint64_t value;
			uint64_t reserved[7];
		} xxhash64;
		struct {
			char data[24];
			char reserved[40];
		} sha192;
		struct {
			char data[32];
			char reserved[32];
		} sha256;
		struct {
			char data[64];
		} sha512;

		struct {
			uint32_t icrc32;
			uint32_t bigmask;
			uint64_t avail;
			char reserved[48];
		} freemap;
	} check;
} __packed;

typedef struct hammer2_blockref hammer2_blockref_t;

#define HAMMER2_BLOCKREF_BYTES		128
#define HAMMER2_BLOCKREF_RADIX		7

#define HAMMER2_BLOCKREF_LEAF_MAX	65535

#define HAMMER2_BREF_TYPE_EMPTY		0
#define HAMMER2_BREF_TYPE_INODE		1
#define HAMMER2_BREF_TYPE_INDIRECT	2
#define HAMMER2_BREF_TYPE_DATA		3
#define HAMMER2_BREF_TYPE_DIRENT	4
#define HAMMER2_BREF_TYPE_FREEMAP_NODE	5
#define HAMMER2_BREF_TYPE_FREEMAP_LEAF	6
#define HAMMER2_BREF_TYPE_INVALID	7
#define HAMMER2_BREF_TYPE_FREEMAP	254
#define HAMMER2_BREF_TYPE_VOLUME	255

#define HAMMER2_BREF_FLAG_PFSROOT	0x01
#define HAMMER2_BREF_FLAG_UNUSED	0x02
#define HAMMER2_BREF_FLAG_EMERG_MIP	0x04

#define HAMMER2_ENC_CHECK(n)		(((n) & 15) << 4)
#define HAMMER2_DEC_CHECK(n)		(((n) >> 4) & 15)
#define HAMMER2_ENC_COMP(n)		((n) & 15)
#define HAMMER2_DEC_COMP(n)		((n) & 15)

#define HAMMER2_CHECK_NONE		0
#define HAMMER2_CHECK_DISABLED		1
#define HAMMER2_CHECK_ISCSI32		2
#define HAMMER2_CHECK_XXHASH64		3
#define HAMMER2_CHECK_SHA192		4
#define HAMMER2_CHECK_FREEMAP		5

#define HAMMER2_CHECK_DEFAULT		HAMMER2_CHECK_XXHASH64

#define HAMMER2_ENC_ALGO(n)		(n)
#define HAMMER2_DEC_ALGO(n)		((n) & 15)
#define HAMMER2_ENC_LEVEL(n)		((n) << 4)
#define HAMMER2_DEC_LEVEL(n)		(((n) >> 4) & 15)

#define HAMMER2_COMP_NONE		0
#define HAMMER2_COMP_AUTOZERO		1
#define HAMMER2_COMP_LZ4		2
#define HAMMER2_COMP_ZLIB		3

#define HAMMER2_COMP_NEWFS_DEFAULT	HAMMER2_COMP_LZ4

struct hammer2_blockset {
	hammer2_blockref_t	blockref[HAMMER2_SET_COUNT];
};

typedef struct hammer2_blockset hammer2_blockset_t;

#if (1 << HAMMER2_SET_RADIX) != HAMMER2_SET_COUNT
#error "hammer2 direct radix is incorrect"
#endif
#if (1 << HAMMER2_PBUFRADIX) != HAMMER2_PBUFSIZE
#error "HAMMER2_PBUFRADIX and HAMMER2_PBUFSIZE are inconsistent"
#endif
#if (1 << HAMMER2_RADIX_MIN) != HAMMER2_ALLOC_MIN
#error "HAMMER2_RADIX_MIN and HAMMER2_ALLOC_MIN are inconsistent"
#endif

struct hammer2_bmap_data {
	int32_t linear;
	uint16_t class;
	uint8_t reserved06;
	uint8_t reserved07;
	uint32_t reserved08;
	uint32_t reserved0C;
	uint32_t reserved10;
	uint32_t reserved14;
	uint32_t reserved18;
	uint32_t avail;
	uint32_t reserved20[8];

	hammer2_bitmap_t bitmapq[HAMMER2_BMAP_ELEMENTS];
} __packed;

typedef struct hammer2_bmap_data hammer2_bmap_data_t;

#define HAMMER2_INODE_BYTES		1024
#define HAMMER2_INODE_MAXNAME		256
#define HAMMER2_INODE_VERSION_ONE	1

#define HAMMER2_INODE_START		1024

struct hammer2_inode_meta {
	uint16_t	version;
	uint8_t		reserved02;
	uint8_t		pfs_subtype;

	uint32_t	uflags;
	uint32_t	rmajor;
	uint32_t	rminor;
	uint64_t	ctime;
	uint64_t	mtime;
	uint64_t	atime;
	uint64_t	btime;
	hammer2_uuid_t	uid;
	hammer2_uuid_t	gid;

	uint8_t		type;
	uint8_t		op_flags;
	uint16_t	cap_flags;
	uint32_t	mode;

	hammer2_tid_t	inum;
	hammer2_off_t	size;
	uint64_t	nlinks;
	hammer2_tid_t	iparent;
	hammer2_key_t	name_key;
	uint16_t	name_len;
	uint8_t		ncopies;
	uint8_t		comp_algo;

	uint8_t		target_type;
	uint8_t		check_algo;
	uint8_t		pfs_nmasters;
	uint8_t		pfs_type;
	hammer2_tid_t	pfs_inum;
	hammer2_uuid_t	pfs_clid;
	hammer2_uuid_t	pfs_fsid;

	hammer2_key_t	data_quota;
	hammer2_key_t	unusedB8;
	hammer2_key_t	inode_quota;
	hammer2_key_t	unusedC8;

	hammer2_tid_t	pfs_lsnap_tid;
	hammer2_tid_t	reservedD8;

	uint64_t	decrypt_check;
	hammer2_off_t	reservedE8[3];
} __packed;

typedef struct hammer2_inode_meta hammer2_inode_meta_t;

struct hammer2_inode_data {
	hammer2_inode_meta_t	meta;
	unsigned char	filename[HAMMER2_INODE_MAXNAME];

	union {
		hammer2_blockset_t blockset;
		char data[HAMMER2_EMBEDDED_BYTES];
	} u;
} __packed;

typedef struct hammer2_inode_data hammer2_inode_data_t;

#define HAMMER2_OPFLAG_DIRECTDATA	0x01
#define HAMMER2_OPFLAG_PFSROOT		0x02
#define HAMMER2_OPFLAG_COPYIDS		0x04

#define HAMMER2_OBJTYPE_UNKNOWN		0
#define HAMMER2_OBJTYPE_DIRECTORY	1
#define HAMMER2_OBJTYPE_REGFILE		2
#define HAMMER2_OBJTYPE_FIFO		4
#define HAMMER2_OBJTYPE_CDEV		5
#define HAMMER2_OBJTYPE_BDEV		6
#define HAMMER2_OBJTYPE_SOFTLINK	7
#define HAMMER2_OBJTYPE_UNUSED08	8
#define HAMMER2_OBJTYPE_SOCKET		9
#define HAMMER2_OBJTYPE_WHITEOUT	10

#define HAMMER2_COPYID_NONE		0
#define HAMMER2_COPYID_LOCAL		((uint8_t)-1)

#define HAMMER2_COPYID_COUNT		256

#define HAMMER2_PFSTYPE_NONE		0x00
#define HAMMER2_PFSTYPE_CACHE		0x01
#define HAMMER2_PFSTYPE_UNUSED02	0x02
#define HAMMER2_PFSTYPE_SLAVE		0x03
#define HAMMER2_PFSTYPE_SOFT_SLAVE	0x04
#define HAMMER2_PFSTYPE_SOFT_MASTER	0x05
#define HAMMER2_PFSTYPE_MASTER		0x06
#define HAMMER2_PFSTYPE_UNUSED07	0x07
#define HAMMER2_PFSTYPE_SUPROOT		0x08
#define HAMMER2_PFSTYPE_DUMMY		0x09
#define HAMMER2_PFSTYPE_MAX		16

#define HAMMER2_PFSTRAN_NONE		0x00
#define HAMMER2_PFSTRAN_CACHE		0x10
#define HAMMER2_PFSTRAN_UNUSED20	0x20
#define HAMMER2_PFSTRAN_SLAVE		0x30
#define HAMMER2_PFSTRAN_SOFT_SLAVE	0x40
#define HAMMER2_PFSTRAN_SOFT_MASTER	0x50
#define HAMMER2_PFSTRAN_MASTER		0x60
#define HAMMER2_PFSTRAN_UNUSED70	0x70
#define HAMMER2_PFSTRAN_SUPROOT		0x80
#define HAMMER2_PFSTRAN_DUMMY		0x90

#define HAMMER2_PFS_DEC(n)		((n) & 0x0F)
#define HAMMER2_PFS_DEC_TRANSITION(n)	(((n) >> 4) & 0x0F)
#define HAMMER2_PFS_ENC_TRANSITION(n)	(((n) & 0x0F) << 4)

#define HAMMER2_PFSSUBTYPE_NONE		0
#define HAMMER2_PFSSUBTYPE_SNAPSHOT	1
#define HAMMER2_PFSSUBTYPE_AUTOSNAP	2

#define HAMMER2_PFSMODE_QUORUM		0x01
#define HAMMER2_PFSMODE_RW		0x02

#define HAMMER2_VOLUME_ID_HBO	0x48414d3205172011LLU
#define HAMMER2_VOLUME_ID_ABO	0x11201705324d4148LLU

#define HAMMER2_MAX_VOLUMES	64
#define HAMMER2_ROOT_VOLUME	0

struct hammer2_volume_data {

	uint64_t	magic;
	hammer2_off_t	boot_beg;
	hammer2_off_t	boot_end;
	hammer2_off_t	aux_beg;
	hammer2_off_t	aux_end;
	hammer2_off_t	volu_size;

	uint32_t	version;
	uint32_t	flags;
	uint8_t		copyid;
	uint8_t		freemap_version;
	uint8_t		peer_type;
	uint8_t		volu_id;
	uint8_t		nvolumes;
	uint8_t		reserved003D;
	uint16_t	reserved003E;

	hammer2_uuid_t	fsid;
	hammer2_uuid_t	fstype;

	hammer2_off_t	allocator_size;
	hammer2_off_t   allocator_free;
	hammer2_off_t	allocator_beg;

	hammer2_tid_t	mirror_tid;
	hammer2_tid_t	reserved0080;
	hammer2_tid_t	reserved0088;
	hammer2_tid_t	freemap_tid;
	hammer2_tid_t	bulkfree_tid;
	hammer2_tid_t	reserved00A0[4];

	hammer2_off_t	total_size;

	uint32_t	copyexists[8];
	char		reserved00E8[248];

	hammer2_crc32_t	icrc_sects[8];

	hammer2_blockset_t sroot_blockset;

	char	sector2[512];
	char	sector3[512];
	hammer2_blockset_t freemap_blockset;
	char	sector5[512];
	char	sector6[512];

	hammer2_off_t volu_loff[HAMMER2_MAX_VOLUMES];

	hammer2_volconf_t copyinfo[HAMMER2_COPYID_COUNT];

	char		reserved9000[0x6FFC];

	hammer2_crc32_t	icrc_volheader;
} __packed;

typedef struct hammer2_volume_data hammer2_volume_data_t;

#define HAMMER2_VOL_ICRC_SECT0		7
#define HAMMER2_VOL_ICRC_SECT1		6

#define HAMMER2_VOLUME_BYTES		65536

#define HAMMER2_VOLUME_ICRC0_OFF	0
#define HAMMER2_VOLUME_ICRC1_OFF	512
#define HAMMER2_VOLUME_ICRCVH_OFF	0

#define HAMMER2_VOLUME_ICRC0_SIZE	(512 - 4)
#define HAMMER2_VOLUME_ICRC1_SIZE	(512)
#define HAMMER2_VOLUME_ICRCVH_SIZE	(65536 - 4)

#define HAMMER2_VOL_VERSION_MULTI_VOLUMES	2

#define HAMMER2_VOL_VERSION_MIN		1
#define HAMMER2_VOL_VERSION_DEFAULT	HAMMER2_VOL_VERSION_MULTI_VOLUMES
#define HAMMER2_VOL_VERSION_WIP		(HAMMER2_VOL_VERSION_MULTI_VOLUMES + 1)

#define HAMMER2_NUM_VOLHDRS		4

union hammer2_media_data {
	hammer2_volume_data_t	voldata;
        hammer2_inode_data_t    ipdata;
	hammer2_blockset_t	blkset;
	hammer2_blockref_t	npdata[HAMMER2_IND_COUNT_MAX];
	hammer2_bmap_data_t	bmdata[HAMMER2_FREEMAP_COUNT];
	char			buf[HAMMER2_PBUFSIZE];
} __packed;

typedef union hammer2_media_data hammer2_media_data_t;

#endif
