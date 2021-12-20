#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>

#include "trace_time.h"

struct test_rcu {
    struct task_struct *task;
    int tid;
};

struct test {
    int val;
};

static struct test __rcu *gp;

static int read_side(void *data)
{
    struct test *cur;
    struct test_rcu *tr = (struct test_rcu *)data;

    rcu_read_lock();

    cur = rcu_dereference(gp);
    pr_info("[tid %d] read %d\n", tr->tid, cur->val);

    rcu_read_unlock();

    return 0;
}

struct trace_time trace_update;
spinlock_t sp = __SPIN_LOCK_UNLOCKED(sp);

static int update_side(void *data)
{
    struct test *oldp, *newp;
    struct test_rcu *tr = (struct test_rcu *)data;

    newp = (struct test *)kmalloc(sizeof(struct test), GFP_KERNEL);
    if (newp == NULL)
        return -ENOMEM;
    newp->val = tr->tid;

    spin_lock(&sp);
    oldp = READ_ONCE(gp);
    rcu_assign_pointer(gp, newp);
    spin_unlock(&sp);

    TRACE_TIME_START(trace_update);
    synchronize_rcu();
    TRACE_TIME_END(trace_update);
    TRACE_CALC(trace_update);
    TRACE_PRINT(trace_update);
    kfree(oldp);

    return 0;
}

#define NR_READ_SIDE 20
#define NR_UPDATE_BESIDE 5
#define NR_TOTAL (NR_READ_SIDE + (NR_READ_SIDE / NR_UPDATE_BESIDE))
static struct test_rcu *t;

static int __init test_rcu_init(void)
{
    int i, *setp;

    t = (struct test_rcu *)kmalloc(sizeof(struct test_rcu) * NR_TOTAL,
                                    GFP_KERNEL);
    if (t == NULL) {
        pr_alert("lrcu_init: t kmalloc failed\n");
        return -ENOMEM;
    }

    gp = (struct test __rcu *)kmalloc(sizeof(struct test), GFP_KERNEL);
    if (gp == NULL) {
        pr_alert("rcu_init: gp kmalloc failed\n");
        return -ENOMEM;
    }

    setp = (int __force *)&gp->val;
    *setp = -1;

    trace_update = TRACE_TIME_INIT("trace rcu");

    for (i = 0; i < NR_TOTAL; i++) {
        t[i].tid = i;
        if (i % NR_UPDATE_BESIDE == 0) {
            t[i].task = kthread_create(update_side, (void *)&t[i],
                                       "kthread: LRCU update side");
        } else {
            t[i].task = kthread_create(read_side, (void *)&t[i],
                                       "kthread: LRCU read side");
        }
    }


    for (i = 0; i < NR_TOTAL; i++)
        wake_up_process(t[i].task);

    return 0;
}

static void __exit test_rcu_exit(void)
{
    synchronize_rcu();
    kfree((int __force *)gp);
    kfree(t);
}

module_init(test_rcu_init);
module_exit(test_rcu_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("This is test_rcu module");
