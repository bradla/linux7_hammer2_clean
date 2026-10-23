#ifndef _HAMMER2_OS_H_
#define _HAMMER2_OS_H_

#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/errno.h>

struct hammer2_mtx_wrapper {
	struct mutex		lock;
	struct task_struct	*owner;
	int			depth;
};
typedef struct hammer2_mtx_wrapper	hammer2_mtx_t;

typedef struct rw_semaphore		hammer2_lk_t;
typedef wait_queue_head_t		hammer2_lkc_t;
typedef spinlock_t			hammer2_spin_t;

static inline void
hammer2_mtx_init(hammer2_mtx_t *p, const char *s)
{
	mutex_init(&p->lock);
	p->owner = NULL;
	p->depth = 0;
}

static inline void
hammer2_mtx_init_recurse(hammer2_mtx_t *p, const char *s)
{
	hammer2_mtx_init(p, s);
}

static inline void
hammer2_mtx_ex(hammer2_mtx_t *p)
{
	if (READ_ONCE(p->owner) == current) {
		++p->depth;
		return;
	}
	mutex_lock(&p->lock);
	WRITE_ONCE(p->owner, current);
	p->depth = 1;
}

static inline void
hammer2_mtx_sh(hammer2_mtx_t *p)
{
	hammer2_mtx_ex(p);
}

static inline void
hammer2_mtx_unlock(hammer2_mtx_t *p)
{
	if (--p->depth == 0) {
		WRITE_ONCE(p->owner, NULL);
		mutex_unlock(&p->lock);
	}
}

static inline int
hammer2_mtx_refs(hammer2_mtx_t *p)
{
	return p->depth;
}

static inline void
hammer2_mtx_destroy(hammer2_mtx_t *p)
{
	mutex_destroy(&p->lock);
}

static inline int
hammer2_mtx_sleep(void *c, hammer2_mtx_t *p, const char *s, int timo)
{
	int depth = p->depth;

	(void)c;
	(void)s;

	p->depth = 0;
	WRITE_ONCE(p->owner, NULL);
	mutex_unlock(&p->lock);

	schedule_timeout_uninterruptible(timo ? timo : 1);

	mutex_lock(&p->lock);
	WRITE_ONCE(p->owner, current);
	p->depth = depth;
	return 0;
}

static inline void
hammer2_mtx_wakeup(void *c)
{
	(void)c;

}

static inline int
hammer2_spin_ssleep(const volatile void *ident, hammer2_spin_t *spin,
    int flags, const char *wmesg, int timo)
{
	(void)ident;
	(void)flags;
	(void)wmesg;

	spin_unlock(spin);
	schedule_timeout_uninterruptible(timo ? timo : 1);
	spin_lock(spin);
	return 0;
}

static inline int
hammer2_mtx_owned(hammer2_mtx_t *p)
{
	return READ_ONCE(p->owner) == current;
}

static inline int
hammer2_mtx_ex_try(hammer2_mtx_t *p)
{
	if (READ_ONCE(p->owner) == current) {
		++p->depth;
		return 0;
	}
	if (mutex_trylock(&p->lock)) {
		WRITE_ONCE(p->owner, current);
		p->depth = 1;
		return 0;
	}
	return 1;
}

static inline int
hammer2_mtx_sh_try(hammer2_mtx_t *p)
{
	return hammer2_mtx_ex_try(p);
}

static inline int
hammer2_mtx_upgrade_try(hammer2_mtx_t *p)
{

	(void)p;
	return 0;
}

static inline int
hammer2_mtx_temp_release(hammer2_mtx_t *p)
{
	int x = p->depth;

	p->depth = 0;
	WRITE_ONCE(p->owner, NULL);
	mutex_unlock(&p->lock);
	return x;
}

static inline void
hammer2_mtx_temp_restore(hammer2_mtx_t *p, int x)
{
	mutex_lock(&p->lock);
	WRITE_ONCE(p->owner, current);
	p->depth = x;
}

#define hammer2_mtx_lock(p)	hammer2_mtx_ex(p)
#define hammer2_mtx_shunlock(p)	hammer2_mtx_unlock(p)

#define hammer2_spin_ex(s)	spin_lock(s)
#define hammer2_spin_unex(s)	spin_unlock(s)
#define hammer2_spin_sh(s)	spin_lock(s)
#define hammer2_spin_unsh(s)	spin_unlock(s)

#define hammer2_lk_ex(l)	down_write(l)
#define hammer2_lk_unlock(l)	up_write(l)
#define hammer2_lk_assert_ex(l)	WARN_ON(!rwsem_is_locked(l))

#define hammer2_mtx_assert_unlocked(p)	WARN_ON(READ_ONCE((p)->owner) == current)

static inline int
hammer2_lkc_sleep(hammer2_lkc_t *c, hammer2_lk_t *lk, const char *s, int timo)
{
	DEFINE_WAIT(__w);
	long rem = 1;

	(void)s;

	prepare_to_wait(c, &__w, TASK_UNINTERRUPTIBLE);
	up_write(lk);
	if (timo == 0)
		schedule();
	else
		rem = schedule_timeout(msecs_to_jiffies(timo));
	finish_wait(c, &__w);
	down_write(lk);

	return (timo && rem == 0) ? -ETIMEDOUT : 0;
}

static inline void
hammer2_lkc_wakeup(hammer2_lkc_t *c)
{
	wake_up(c);
}

static inline void
hammer2_lkc_init(hammer2_lkc_t *c, const char *s)
{
	init_waitqueue_head(c);
}

static inline void
hammer2_lkc_destroy(hammer2_lkc_t *c)
{
	(void)c;
}

#endif
