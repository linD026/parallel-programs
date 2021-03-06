/* 
 * Little Read Copy Update: A test of Classic RCU
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 * 
 * Copyright (C) 2021 linD026
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/kthread.h>

#include "trace_time.h"
#include "rcu.h"

struct test_lrcu {
    struct lrcu_data *ld;
    struct task_struct *task;
    int tid;
};

struct test {
    int val;
};

static struct test __lrcu *gp;

static int read_side(void *data)
{
    struct test *cur;
    struct test_lrcu *test_info = (struct test_lrcu *)data;

    lrcu_read_lock();

    cur = lrcu_dereference(gp);
    pr_info("[tid %d] read %d\n", test_info->tid, cur->val);

    lrcu_read_unlock();

    return 0;
}

struct trace_time trace_update;

static int update_side(void *data)
{
    struct test *oldp, *newp;
    struct test_lrcu *test_info = (struct test_lrcu *)data;

    newp = (struct test *)kmalloc(sizeof(struct test), GFP_KERNEL);
    if (newp == NULL) {
        pr_alert("update_side %d:kmalloc failed\n", test_info->tid);
        return -ENOMEM;
    }
    newp->val = test_info->tid;

    oldp = lrcu_assign_pointer(gp, newp, test_info->ld);
    if (oldp == NULL)
        return -1;

    TRACE_TIME_START(trace_update);
    synchronize_lrcu(test_info->ld);
    TRACE_TIME_END(trace_update);
    TRACE_CALC(trace_update);
    TRACE_PRINT(trace_update);
    kfree(oldp);

    return 0;
}

static void lrcu_call_back(void *data)
{
    kfree(data);
}

#define NR_READ_SIDE 20
#define NR_UPDATE_BESIDE 5
#define NR_TOTAL (NR_READ_SIDE + (NR_READ_SIDE / NR_UPDATE_BESIDE))
struct lrcu_data *lrcu_data;
static struct test_lrcu *t;

static int __init lrcu_init(void)
{
    int i, *setp;

    lrcu_data = lrcu_data_init(lrcu_call_back);
    if (!lrcu_data)
        return -1;

    i = lrcu_sched_init();
    if (i != 0)
        return -1;

    t = (struct test_lrcu *)kmalloc(sizeof(struct test_lrcu) * NR_TOTAL,
                                    GFP_KERNEL);
    if (t == NULL) {
        pr_alert("lrcu_init: t kmalloc failed\n");
        return -ENOMEM;
    }

    gp = (struct test __lrcu *)kmalloc(sizeof(struct test), GFP_KERNEL);
    if (gp == NULL) {
        pr_alert("lrcu_init: gp kmalloc failed\n");
        return -ENOMEM;
    }

    setp = (int __force *)&gp->val;
    *setp = -1;

    trace_update = TRACE_TIME_INIT("trace lrcu");

    for (i = 0; i < NR_TOTAL; i++) {
        t[i].tid = i;
        t[i].ld = lrcu_data;
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

    call_lrcu(lrcu_data);

    return 0;
}

static void __exit lrcu_exit(void)
{
    lrcu_assign_pointer(gp, NULL, lrcu_data);

    synchronize_lrcu(lrcu_data);
    kfree(lrcu_data);
    kfree(t);
}

module_init(lrcu_init);
module_exit(lrcu_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("This is test_lrcu module");
