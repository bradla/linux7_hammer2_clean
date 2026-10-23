#include "hammer2.h"

#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>

void
hammer2_thr_signal(hammer2_thread_t *thr, uint32_t flags)
{
	uint32_t oflags;
	uint32_t nflags;

	for (;;) {
		oflags = thr->flags;
		cpu_ccfence();
		nflags = (oflags | flags) & ~HAMMER2_THREAD_WAITING;

		if (atomic_cmpset_int(&thr->flags, oflags, nflags)) {
			if (oflags & HAMMER2_THREAD_WAITING)
				wake_up(&thr->wait);
			break;
		}
	}
}

void
hammer2_thr_signal2(hammer2_thread_t *thr, uint32_t posflags, uint32_t negflags)
{
	uint32_t oflags;
	uint32_t nflags;

	for (;;) {
		oflags = thr->flags;
		cpu_ccfence();
		nflags = (oflags | posflags) &
			~(negflags | HAMMER2_THREAD_WAITING);
		if (atomic_cmpset_int(&thr->flags, oflags, nflags)) {
			if (oflags & HAMMER2_THREAD_WAITING)
				wake_up(&thr->wait);
			break;
		}
	}
}

void
hammer2_thr_wait(hammer2_thread_t *thr, uint32_t flags)
{
	wait_event(thr->wait, (READ_ONCE(thr->flags) & flags) == flags);
}

int
hammer2_thr_wait_any(hammer2_thread_t *thr, uint32_t flags, int timo)
{
	long ret;

	if (timo == 0) {
		wait_event(thr->wait, (READ_ONCE(thr->flags) & flags) != 0);
		return 0;
	}
	ret = wait_event_timeout(thr->wait,
				 (READ_ONCE(thr->flags) & flags) != 0, timo);
	if (ret == 0)
		return HAMMER2_ERROR_ETIMEDOUT;
	return 0;
}

void
hammer2_thr_wait_neg(hammer2_thread_t *thr, uint32_t flags)
{
	wait_event(thr->wait, (READ_ONCE(thr->flags) & flags) == 0);
}

static int
hammer2_thr_trampoline(void *data)
{
	hammer2_thread_t *thr = data;

	thr->func(thr);
	return 0;
}

void
hammer2_thr_create(hammer2_thread_t *thr, hammer2_pfs_t *pmp,
		   hammer2_dev_t *hmp,
		   const char *id, int clindex, int repidx,
		   void (*func)(void *arg))
{
	struct task_struct *task;

	thr->pmp = pmp;
	thr->hmp = hmp;
	thr->clindex = clindex;
	thr->repidx = repidx;
	thr->func = func;
	init_waitqueue_head(&thr->wait);
	atomic_clear_int(&thr->flags, HAMMER2_THREAD_STOP |
				      HAMMER2_THREAD_STOPPED |
				      HAMMER2_THREAD_FREEZE |
				      HAMMER2_THREAD_FROZEN);
	if (thr->scratch == NULL)
		thr->scratch = hmalloc(MAXPHYS, M_HAMMER2, M_WAITOK | M_ZERO);

	if (repidx >= 0) {
		task = kthread_run(hammer2_thr_trampoline, thr, "%s-%s.%02d",
				   id, pmp->pfs_names[clindex], repidx);
	} else if (pmp) {
		task = kthread_run(hammer2_thr_trampoline, thr, "%s-%s",
				   id, pmp->pfs_names[clindex]);
	} else {
		task = kthread_run(hammer2_thr_trampoline, thr, "%s", id);
	}

	if (IS_ERR(task)) {
		thr->task = NULL;
		thr->td = NULL;
		hprintf("failed to start thread %s: %ld\n", id, PTR_ERR(task));
		return;
	}
	thr->task = task;
	thr->td = (thread_t)task;
}

void
hammer2_thr_delete(hammer2_thread_t *thr)
{
	if (thr->td == NULL)
		return;
	hammer2_thr_signal(thr, HAMMER2_THREAD_STOP);
	hammer2_thr_wait(thr, HAMMER2_THREAD_STOPPED);
	thr->pmp = NULL;
	thr->task = NULL;
	if (thr->scratch) {
		hfree(thr->scratch, M_HAMMER2, MAXPHYS);
		thr->scratch = NULL;
	}
}

void
hammer2_thr_remaster(hammer2_thread_t *thr)
{
	if (thr->td == NULL)
		return;
	hammer2_thr_signal(thr, HAMMER2_THREAD_REMASTER);
}

void
hammer2_thr_freeze_async(hammer2_thread_t *thr)
{
	hammer2_thr_signal(thr, HAMMER2_THREAD_FREEZE);
}

void
hammer2_thr_freeze(hammer2_thread_t *thr)
{
	if (thr->td == NULL)
		return;
	hammer2_thr_signal(thr, HAMMER2_THREAD_FREEZE);
	hammer2_thr_wait(thr, HAMMER2_THREAD_FROZEN);
}

void
hammer2_thr_unfreeze(hammer2_thread_t *thr)
{
	if (thr->td == NULL)
		return;
	hammer2_thr_signal(thr, HAMMER2_THREAD_UNFREEZE);
	hammer2_thr_wait_neg(thr, HAMMER2_THREAD_FROZEN);
}

int
hammer2_thr_break(hammer2_thread_t *thr)
{
	if (thr->flags & (HAMMER2_THREAD_STOP |
			  HAMMER2_THREAD_REMASTER |
			  HAMMER2_THREAD_FREEZE)) {
		return 1;
	}
	return 0;
}
