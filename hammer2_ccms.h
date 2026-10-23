#ifndef _HAMMER2_CCMS_H_
#define _HAMMER2_CCMS_H_

typedef uint64_t	ccms_key_t;
typedef uint64_t	ccms_tid_t;
typedef uint8_t		ccms_state_t;
typedef uint8_t		ccms_type_t;

struct ccms_cst;

#define CCMS_STATE_INVALID	0
#define CCMS_STATE_SHARED	2
#define CCMS_STATE_EXCLUSIVE	3

#define CCMS_TYPE_INHERITED	0x01
#define CCMS_TYPE_MODIFIED	0x02
#define CCMS_TYPE_MASTER	0x04
#define CCMS_TYPE_SLAVE		0x08
#define CCMS_TYPE_QSALVE	0x10
#define CCMS_TYPE_RECURSIVE	0x80

struct ccms_cst {
	hammer2_spin_t	spin;
	ccms_state_t	state;
	ccms_type_t	type;
	uint8_t		unused02;
	uint8_t		unused03;

	int32_t		upgrade;
	int32_t		count;
	int32_t		blocked;
	thread_t	td;
};

typedef struct ccms_cst		ccms_cst_t;

void ccms_cst_init(ccms_cst_t *cst);
void ccms_cst_uninit(ccms_cst_t *cst);

void ccms_thread_lock(ccms_cst_t *cst, ccms_state_t state);
int ccms_thread_lock_nonblock(ccms_cst_t *cst, ccms_state_t state);
ccms_state_t ccms_thread_lock_temp_release(ccms_cst_t *cst);
void ccms_thread_lock_temp_restore(ccms_cst_t *cst, ccms_state_t ostate);
ccms_state_t ccms_thread_lock_upgrade(ccms_cst_t *cst);
void ccms_thread_lock_downgrade(ccms_cst_t *cst, ccms_state_t ostate);
void ccms_thread_unlock(ccms_cst_t *cst);
void ccms_thread_unlock_upgraded(ccms_cst_t *cst, ccms_state_t ostate);
int ccms_thread_lock_owned(ccms_cst_t *cst);
void ccms_thread_lock_setown(ccms_cst_t *cst);

#endif
