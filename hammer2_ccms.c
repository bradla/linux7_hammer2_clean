#include "hammer2.h"
#include "hammer2_ccms.h"

#undef ssleep
#define ssleep(ident, spin, flags, wmesg, timo) \
	hammer2_spin_ssleep((ident), (spin), (flags), (wmesg), (timo))

int ccms_debug = 0;

void
ccms_cst_init(ccms_cst_t *cst)
{
	bzero(cst, sizeof(*cst));
	hammer2_spin_init(&cst->spin, "ccmscst");
}

void
ccms_cst_uninit(ccms_cst_t *cst)
{
	KKASSERT(cst->count == 0);
	if (cst->state != CCMS_STATE_INVALID) {

	}
}

void
ccms_thread_lock(ccms_cst_t *cst, ccms_state_t state)
{

	LOCKENTER;
	if (cst->count < 0 && cst->td == curthread) {
		--cst->count;
		return;
	}

	hammer2_spin_ex(&cst->spin);
	if (state == CCMS_STATE_SHARED) {
		while (cst->count < 0 || cst->upgrade) {
			cst->blocked = 1;
			ssleep(cst, &cst->spin, 0, "ccmslck", hz);
		}
		++cst->count;
		KKASSERT(cst->td == NULL);
	} else if (state == CCMS_STATE_EXCLUSIVE) {
		while (cst->count != 0 || cst->upgrade) {
			cst->blocked = 1;
			ssleep(cst, &cst->spin, 0, "ccmslck", hz);
		}
		cst->count = -1;
		cst->td = curthread;
	} else {
		hammer2_spin_unex(&cst->spin);
		panic("ccms_thread_lock: bad state %d\n", state);
	}
	hammer2_spin_unex(&cst->spin);
}

int
ccms_thread_lock_nonblock(ccms_cst_t *cst, ccms_state_t state)
{
	if (cst->count < 0 && cst->td == curthread) {
		--cst->count;
		LOCKENTER;
		return(0);
	}

	hammer2_spin_ex(&cst->spin);
	if (state == CCMS_STATE_SHARED) {
		if (cst->count < 0 || cst->upgrade) {
			hammer2_spin_unex(&cst->spin);
			return (EBUSY);
		}
		++cst->count;
		KKASSERT(cst->td == NULL);
	} else if (state == CCMS_STATE_EXCLUSIVE) {
		if (cst->count != 0 || cst->upgrade) {
			hammer2_spin_unex(&cst->spin);
			return (EBUSY);
		}
		cst->count = -1;
		cst->td = curthread;
	} else {
		hammer2_spin_unex(&cst->spin);
		panic("ccms_thread_lock_nonblock: bad state %d\n", state);
	}
	hammer2_spin_unex(&cst->spin);
	LOCKENTER;
	return(0);
}

ccms_state_t
ccms_thread_lock_temp_release(ccms_cst_t *cst)
{
	if (cst->count < 0) {
		ccms_thread_unlock(cst);
		return(CCMS_STATE_EXCLUSIVE);
	}
	if (cst->count > 0) {
		ccms_thread_unlock(cst);
		return(CCMS_STATE_SHARED);
	}
	return (CCMS_STATE_INVALID);
}

void
ccms_thread_lock_temp_restore(ccms_cst_t *cst, ccms_state_t ostate)
{
	ccms_thread_lock(cst, ostate);
}

ccms_state_t
ccms_thread_lock_upgrade(ccms_cst_t *cst)
{

	if (cst->count < 0) {
		KKASSERT(cst->td == curthread);
		return(CCMS_STATE_EXCLUSIVE);
	}

	if (cst->count > 0) {
		hammer2_spin_ex(&cst->spin);
		++cst->upgrade;
		--cst->count;
		while (cst->count) {
			cst->blocked = 1;
			ssleep(cst, &cst->spin, 0, "ccmsupg", hz);
		}
		cst->count = -1;
		cst->td = curthread;
		hammer2_spin_unex(&cst->spin);
		return(CCMS_STATE_SHARED);
	}
	panic("ccms_thread_lock_upgrade: not locked");

	return(0);
}

void
ccms_thread_lock_downgrade(ccms_cst_t *cst, ccms_state_t ostate)
{
	if (ostate == CCMS_STATE_SHARED) {
		KKASSERT(cst->td == curthread);
		KKASSERT(cst->count == -1);
		hammer2_spin_ex(&cst->spin);
		--cst->upgrade;
		cst->count = 1;
		cst->td = NULL;
		if (cst->blocked) {
			cst->blocked = 0;
			hammer2_spin_unex(&cst->spin);
			wakeup(cst);
		} else {
			hammer2_spin_unex(&cst->spin);
		}
	}

}

void
ccms_thread_unlock(ccms_cst_t *cst)
{
	LOCKEXIT;
	if (cst->count < 0) {

		KKASSERT(cst->td == curthread);
		if (cst->count < -1) {
			++cst->count;
			return;
		}
		hammer2_spin_ex(&cst->spin);
		KKASSERT(cst->count == -1);
		cst->count = 0;
		cst->td = NULL;
		if (cst->blocked) {
			cst->blocked = 0;
			hammer2_spin_unex(&cst->spin);
			wakeup(cst);
			return;
		}
		hammer2_spin_unex(&cst->spin);
	} else if (cst->count > 0) {

		hammer2_spin_ex(&cst->spin);
		if (--cst->count == 0 && cst->blocked) {
			cst->blocked = 0;
			hammer2_spin_unex(&cst->spin);
			wakeup(cst);
			return;
		}
		hammer2_spin_unex(&cst->spin);
	} else {
		panic("ccms_thread_unlock: bad zero count\n");
	}
}

void
ccms_thread_lock_setown(ccms_cst_t *cst)
{
	KKASSERT(cst->count < 0);
	cst->td = curthread;
}

void
ccms_thread_unlock_upgraded(ccms_cst_t *cst, ccms_state_t ostate)
{
	if (ostate == CCMS_STATE_SHARED) {
		LOCKEXIT;
		KKASSERT(cst->td == curthread);
		KKASSERT(cst->count == -1);
		hammer2_spin_ex(&cst->spin);
		--cst->upgrade;
		cst->count = 0;
		cst->td = NULL;
		if (cst->blocked) {
			cst->blocked = 0;
			hammer2_spin_unex(&cst->spin);
			wakeup(cst);
		} else {
			hammer2_spin_unex(&cst->spin);
		}
	} else {
		ccms_thread_unlock(cst);
	}
}

int
ccms_thread_lock_owned(ccms_cst_t *cst)
{
	return(cst->count < 0 && cst->td == curthread);
}
