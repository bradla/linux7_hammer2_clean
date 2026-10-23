#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>

#include "hammer2.h"
#include "hammer2_bsdvfs.h"

const struct vop_vector default_vnodeops;
const struct vop_vector fifo_specops;
int vop_stdfdatasync_buf(struct vop_fsync_args *ap)
{
	(void)ap;
	return 0;
}
int hammer2_vop_panic(void *ap)
{
	(void)ap;
	pr_err("hammer2: VOP_PANIC slot invoked -- VFS dispatch not wired\n");
	return -EIO;
}

MODULE_LICENSE("Dual BSD/GPL");
MODULE_DESCRIPTION("DragonFlyBSD HAMMER2 filesystem port");
MODULE_AUTHOR("HAMMER2 contributors");
