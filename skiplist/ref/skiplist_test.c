/*
 * (C) 2013 Fusion-io
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/kthread.h>
#include <linux/rbtree.h>
#include <linux/random.h>

#include <linux/time.h>

#include "skiplist.h"

static int threads = 1;
static int rounds = 100;
static int items = 100;
static int module_exiting;
static struct completion startup = COMPLETION_INITIALIZER(startup);
static DEFINE_MUTEX(fill_mutex);
static int filled;

static struct timespec64 *times;

#define FILL_TIME_INDEX 0
#define CHECK_TIME_INDEX 1
#define DEL_TIME_INDEX 2
#define FIRST_THREAD_INDEX 3

#define SKIPLIST_RCU_BENCH 1
#define SKIPLIST_BENCH 2
#define RBTREE_BENCH 3

static int benchmark = SKIPLIST_RCU_BENCH;

module_param(threads, int, 0);
module_param(rounds, int, 0);
module_param(items, int, 0);
module_param(benchmark, int, 0);

MODULE_PARM_DESC(threads, "number of threads to run");
MODULE_PARM_DESC(rounds, "how many random operations to run");
MODULE_PARM_DESC(items, "number of items to fill the list with");
MODULE_PARM_DESC(benchmark,
                 "benchmark to run 1=skiplist-rcu 2=skiplist-locking 3=rbtree");
MODULE_LICENSE("GPL");

static atomic_t threads_running = ATOMIC_INIT(0);

/*
 * since the skiplist code is more concurrent, it is also more likely to
 * have races into the same slot during our delete/insert bashing run.
 * This makes counts the number of delete/insert pairs done so we can
 * make sure the results are roughly accurate
 */
static atomic_t pops_done = ATOMIC_INIT(0);

static struct kmem_cache *slot_cache;
struct sl_list skiplist;

spinlock_t rbtree_lock;
struct rb_root rb_root = RB_ROOT;

struct rbtree_item {
    struct rb_node rb_node;
    unsigned long key;
    unsigned long size;
};

static int __insert_one_rbtree(struct rb_root *root, struct rbtree_item *ins)
{
    struct rb_node **p = &root->rb_node;
    struct rb_node *parent = NULL;
    struct rbtree_item *item;
    unsigned long key = ins->key;
    int ret = -EEXIST;

    while (*p) {
        parent = *p;
        item = rb_entry(parent, struct rbtree_item, rb_node);

        if (key < item->key)
            p = &(*p)->rb_left;
        else if (key >= item->key + item->size)
            p = &(*p)->rb_right;
        else
            goto out;
    }

    rb_link_node(&ins->rb_node, parent, p);
    rb_insert_color(&ins->rb_node, root);
    ret = 0;
out:

    return ret;
}

static int insert_one_rbtree(struct rb_root *root, unsigned long key,
                             unsigned long size)
{
    int ret;
    struct rbtree_item *ins;

    ins = kmalloc(sizeof(*ins), GFP_KERNEL);
    ins->key = key;
    ins->size = size;

    spin_lock(&rbtree_lock);
    ret = __insert_one_rbtree(root, ins);
    spin_unlock(&rbtree_lock);

    if (ret) {
        printk(KERN_CRIT "err %d inserting rbtree key %lu\n", ret, key);
        kfree(ins);
    }
    return ret;
}

static struct rbtree_item *__lookup_one_rbtree(struct rb_root *root,
                                               unsigned long key)
{
    struct rb_node *p = root->rb_node;
    struct rbtree_item *item;
    struct rbtree_item *ret;

    while (p) {
        item = rb_entry(p, struct rbtree_item, rb_node);

        if (key < item->key)
            p = p->rb_left;
        else if (key >= item->key + item->size)
            p = p->rb_right;
        else {
            ret = item;
            goto out;
        }
    }

    ret = NULL;
out:
    return ret;
}

static int lookup_one_rbtree(struct rb_root *root, unsigned long key)
{
    struct rbtree_item *item;
    int ret;

    spin_lock(&rbtree_lock);
    item = __lookup_one_rbtree(root, key);
    if (item)
        ret = 0;
    else
        ret = -ENOENT;
    spin_unlock(&rbtree_lock);

    return ret;
}

static int pop_one_rbtree(struct rb_root *root, unsigned long key)
{
    int ret = 0;
    struct rbtree_item *item;
    struct rbtree_item **victims;
    int nr_victims = 128;
    int found = 0;
    int loops = 0;
    int i;

    nr_victims = min(nr_victims, items / 2);

    victims = kzalloc(nr_victims * sizeof(victims[0]), GFP_KERNEL);
    /*
	 * this is intentionally deleting adjacent items to empty
	 * skiplist leaves.  The goal is to find races between
	 * leaf deletion and the rest of the code
	 */
    while (found < nr_victims && loops < 256) {
        loops++;

        spin_lock(&rbtree_lock);
        item = __lookup_one_rbtree(root, key + loops * 4096);
        if (item) {
            victims[found] = item;
            atomic_inc(&pops_done);
            rb_erase(&item->rb_node, root);
            found++;
        }
        spin_unlock(&rbtree_lock);
        cond_resched();
    }

    for (i = 0; i < found; i++) {
        item = victims[i];
        spin_lock(&rbtree_lock);
        ret = __insert_one_rbtree(root, item);
        if (ret) {
            printk(KERN_CRIT "pop_one unable to insert %lu\n", key);
            kfree(item);
        }
        spin_unlock(&rbtree_lock);
        cond_resched();
    }
    kfree(victims);
    return ret;
}

static int run_initial_fill_rbtree(void)
{
    unsigned long i;
    unsigned long key;
    int ret;
    int inserted = 0;

    sl_init_list(&skiplist, GFP_KERNEL);

    for (i = 0; i < items; i++) {
        key = i * 4096;
        ret = insert_one_rbtree(&rb_root, key, 4096);
        if (ret)
            return ret;
        inserted++;
    }
    printk("rbtree inserted %d items\n", inserted);
    return 0;
}

static void check_post_work_rbtree(void)
{
    unsigned long i;
    unsigned long key;
    int errors = 0;
    int ret;

    for (i = 0; i < items; i++) {
        key = i * 4096;
        ret = lookup_one_rbtree(&rb_root, key);
        if (ret) {
            printk("rbtree failed to find key %lu\n", key);
            errors++;
        }
        cond_resched();
    }
    printk(KERN_CRIT "rbtree check found %d errors\n", errors);
}

static void delete_all_items_rbtree(void)
{
    unsigned long i;
    unsigned long key;
    int mid = items / 2;
    int bounce;
    struct rbtree_item *item;

    for (i = 0; i < mid; i++) {
        bounce = 0;
        key = i * 4096;
    again:
        spin_lock(&rbtree_lock);
        item = __lookup_one_rbtree(&rb_root, key);
        if (!item)
            printk(KERN_CRIT "delete_all unable to find %lu\n", key);
        rb_erase(&item->rb_node, &rb_root);
        spin_unlock(&rbtree_lock);
        kfree(item);

        if (!bounce) {
            key = (items - 1 - i) * 4096;
            bounce = 1;
            goto again;
        }
    }
}

static int insert_one_skiplist(struct sl_list *skiplist, unsigned long key,
                               unsigned long size)
{
    int ret;
    int preload_token;
    struct sl_slot *slot;

    slot = kmem_cache_alloc(slot_cache, GFP_KERNEL);
    if (!slot)
        return -ENOMEM;

    slot->key = key;
    slot->size = size;

    preload_token = skiplist_preload(skiplist, GFP_KERNEL);
    if (preload_token < 0) {
        ret = preload_token;
        goto out;
    }

    ret = skiplist_insert(skiplist, slot, preload_token);
    preempt_enable();

out:
    if (ret)
        kmem_cache_free(slot_cache, slot);

    return ret;
}

static unsigned long tester_random(void)
{
    return prandom_u32();
}

static int run_initial_fill_skiplist(void)
{
    unsigned long i;
    unsigned long key;
    int ret;
    int inserted = 0;

    sl_init_list(&skiplist, GFP_KERNEL);

    for (i = 0; i < items; i++) {
        key = i * 4096;
        ret = insert_one_skiplist(&skiplist, key, 4096);
        if (ret)
            return ret;
        inserted++;
    }
    printk("skiplist inserted %d items\n", inserted);
    return 0;
}

static void check_post_work_skiplist(void)
{
    unsigned long i;
    unsigned long key;
    struct sl_slot *slot;
    int errors = 0;

    for (i = 0; i < items; i++) {
        key = i * 4096;
        if (benchmark == SKIPLIST_RCU_BENCH) {
            rcu_read_lock();
        again:
            slot = skiplist_lookup_rcu(&skiplist, key + 64, 512);
            if (slot && slot->key != key) {
                goto again;
            }
            rcu_read_unlock();
        } else {
            slot = skiplist_lookup(&skiplist, key + 64, 512);
        }

        if (!slot) {
            printk("failed to find key %lu\n", key);
            errors++;
        } else if (slot->key != key) {
            errors++;
            printk("key mismatch wanted %lu found %lu\n", key, slot->key);
        }
        cond_resched();
    }
    printk(KERN_CRIT "skiplist check found %d errors\n", errors);
}

static void verify_post_work_skiplist(void)
{
    unsigned long i;
    unsigned long key = 0;
    struct sl_slot *slot;
    struct sl_node *node = skiplist.head->ptrs[0].next;
    struct sl_leaf *leaf;
    int found = 0;

    while (node) {
        leaf = sl_entry(node);
        for (i = 0; i < leaf->nr; i++) {
            slot = leaf->ptrs[i];
            if (slot->key != key) {
                printk(KERN_CRIT "found bad key %lu wanted %lu\n", slot->key,
                       key);
            }
            key += slot->size;
        }
        found += leaf->nr;
        node = node->ptrs[0].next;
    }
    if (found != items) {
        printk(KERN_CRIT "skiplist check found only %d items instead of %d\n",
               found, items);
    } else {
        printk(KERN_CRIT "skiplist verify passed\n");
    }
}

static void delete_all_items_skiplist(void)
{
    unsigned long i;
    unsigned long key;
    struct sl_slot *slot;
    int errors = 0;
    int mid = items / 2;
    int bounce;

    for (i = 0; i < mid; i++) {
        bounce = 0;
        key = i * 4096;
    again:
        slot = skiplist_delete(&skiplist, key + 512, 1);
        if (!slot) {
            printk("missing key %lu\n", key);
        } else if (slot->key != key) {
            errors++;
            printk("key mismatch wanted %lu found %lu\n", key, slot->key);
        }
        kfree(slot);
        if (!bounce) {
            key = (items - 1 - i) * 4096;
            bounce = 1;
            goto again;
        }
    }
    printk(KERN_CRIT "skiplist deletion done\n");
}

static int lookup_one_skiplist(struct sl_list *skiplist, unsigned long key)
{
    int ret = 0;
    struct sl_slot *slot;

    if (benchmark == SKIPLIST_RCU_BENCH)
        slot = skiplist_lookup_rcu(skiplist, key, 4096);
    else if (benchmark == SKIPLIST_BENCH)
        slot = skiplist_lookup(skiplist, key, 4096);
    if (!slot)
        ret = -ENOENT;
    return ret;
}

static int pop_one_skiplist(struct sl_list *skiplist, unsigned long key)
{
    int ret = 0;
    int preload_token;
    struct sl_slot *slot;
    struct sl_slot **victims;
    int nr_victims = 128;
    int found = 0;
    int loops = 0;
    int i;

    nr_victims = min(nr_victims, items / 2);

    victims = kzalloc(nr_victims * sizeof(victims[0]), GFP_KERNEL);
    /*
	 * this is intentionally deleting adjacent items to empty
	 * skiplist leaves.  The goal is to find races between
	 * leaf deletion and the rest of the code
	 */
    while (found < nr_victims && loops < 256) {
        loops++;
        slot = skiplist_delete(skiplist, key + loops * 4096, 1024);
        if (!slot)
            continue;

        victims[found] = slot;
        atomic_inc(&pops_done);
        found++;
        cond_resched();
    }
    for (i = 0; i < found; i++) {
        preload_token = skiplist_preload(skiplist, GFP_KERNEL);
        if (preload_token < 0) {
            ret = preload_token;
            goto out;
        }

        ret = skiplist_insert(skiplist, victims[i], preload_token);
        if (ret) {
            printk(KERN_CRIT "failed to insert key %lu ret %d\n", key, ret);
            preempt_enable();
            goto out;
        }
        ret = 0;
        preempt_enable();
        cond_resched();
    }

out:
    kfree(victims);
    return ret;
}

void tvsub(struct timespec64 *tdiff, struct timespec64 *t1, struct timespec64 *t0)
{
    tdiff->tv_sec = t1->tv_sec - t0->tv_sec;
    tdiff->tv_nsec = t1->tv_nsec - t0->tv_nsec;
    if (tdiff->tv_nsec < 0 && tdiff->tv_sec > 0) {
        tdiff->tv_sec--;
        tdiff->tv_nsec += 1000000000ULL;
    }

    /* time shouldn't go backwards!!! */
    if (tdiff->tv_nsec < 0 || t1->tv_sec < t0->tv_sec) {
        tdiff->tv_sec = 0;
        tdiff->tv_nsec = 0;
    }
}

static void pretty_time(struct timespec64 *ts, unsigned long long *seconds,
                        unsigned long long *ms)
{
    unsigned long long m;

    *seconds = ts->tv_sec;

    m = ts->tv_nsec / 1000000ULL;
    *ms = m;
}

static void runbench(int thread_index)
{
    int ret = 0;
    unsigned long i;
    unsigned long op;
    unsigned long key;
    struct timespec64 start;
    struct timespec64 cur;
    unsigned long long sec;
    unsigned long long ms;
    char *tag = "skiplist-rcu";

    if (benchmark == SKIPLIST_BENCH)
        tag = "skiplist-locking";
    else if (benchmark == RBTREE_BENCH)
        tag = "rbtree";

    mutex_lock(&fill_mutex);

    if (filled == 0) {
        ktime_get_coarse_real_ts64(&start);

        printk(KERN_CRIT "Running %s benchmark\n", tag);

        if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
            ret = run_initial_fill_skiplist();
        else if (benchmark == RBTREE_BENCH)
            ret = run_initial_fill_rbtree();

        if (ret < 0) {
            printk(KERN_CRIT "failed to setup initial tree ret %d\n", ret);
            filled = ret;
        } else {
            filled = 1;
        }
        ktime_get_coarse_real_ts64(&cur);
        tvsub(times + FILL_TIME_INDEX, &cur, &start);
    }

    mutex_unlock(&fill_mutex);
    if (filled < 0)
        return;

    ktime_get_coarse_real_ts64(&start);

    for (i = 0; i < rounds; i++) {
        op = tester_random();
        key = op % items;
        key *= 4096;
        if (op % 2 == 0) {
            if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
                ret = lookup_one_skiplist(&skiplist, key);
            else if (benchmark == RBTREE_BENCH)
                ret = lookup_one_rbtree(&rb_root, key);
        }
        if (op % 3 == 0) {
            if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
                ret = pop_one_skiplist(&skiplist, key);
            else if (benchmark == RBTREE_BENCH)
                ret = pop_one_rbtree(&rb_root, key);
        }
        cond_resched();
    }

    ktime_get_coarse_real_ts64(&cur);
    tvsub(times + FIRST_THREAD_INDEX + thread_index, &cur, &start);

    if (!atomic_dec_and_test(&threads_running)) {
        return;
    }

    ktime_get_coarse_real_ts64(&start);
    if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
        check_post_work_skiplist();
    else if (benchmark == RBTREE_BENCH)
        check_post_work_rbtree();

    ktime_get_coarse_real_ts64(&cur);

    if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
        verify_post_work_skiplist();

    tvsub(times + CHECK_TIME_INDEX, &cur, &start);

    ktime_get_coarse_real_ts64(&start);
    if (benchmark == SKIPLIST_RCU_BENCH || benchmark == SKIPLIST_BENCH)
        delete_all_items_skiplist();
    else if (benchmark == RBTREE_BENCH)
        delete_all_items_rbtree();
    ktime_get_coarse_real_ts64(&cur);

    tvsub(times + DEL_TIME_INDEX, &cur, &start);

    pretty_time(&times[FILL_TIME_INDEX], &sec, &ms);
    printk("%s fill time %llu s %llu ms\n", tag, sec, ms);
    pretty_time(&times[CHECK_TIME_INDEX], &sec, &ms);
    printk("%s check time %llu s %llu ms\n", tag, sec, ms);
    pretty_time(&times[DEL_TIME_INDEX], &sec, &ms);
    printk("%s del time %llu s %llu ms \n", tag, sec, ms);
    for (i = 0; i < threads; i++) {
        pretty_time(&times[FIRST_THREAD_INDEX + i], &sec, &ms);
        printk("%s thread %lu time %llu s %llu ms\n", tag, i, sec, ms);
    }

    printk("worker thread pops done %d\n", atomic_read(&pops_done));

    kfree(times);
}

static int skiptest_thread(void *index)
{
    unsigned long thread_index = (unsigned long)index;
    complete(&startup);
    runbench(thread_index);
    complete(&startup);
    return 0;
}

static int __init skiptest_init(void)
{
    unsigned long i;
    skiplist_init();
    init_completion(&startup);
    slot_cache = kmem_cache_create(
        "skiplist_slot", sizeof(struct sl_slot), 0,
        SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_TYPESAFE_BY_RCU, NULL);

    if (!slot_cache)
        return -ENOMEM;

    spin_lock_init(&rbtree_lock);

    printk("skiptest benchmark module (%d threads) (%d items) (%d rounds)\n",
           threads, items, rounds);

    times = kmalloc(sizeof(times[0]) * (threads + 3), GFP_KERNEL);

    atomic_set(&threads_running, threads);
    for (i = 0; i < threads; i++) {
        kthread_run(skiptest_thread, (void *)i, "skiptest_thread");
    }
    for (i = 0; i < threads; i++)
        wait_for_completion(&startup);
    return 0;
}

static void __exit skiptest_exit(void)
{
    int i;
    module_exiting = 1;

    for (i = 0; i < threads; i++) {
        wait_for_completion(&startup);
    }

    synchronize_rcu();
    kmem_cache_destroy(slot_cache);
    printk("all skiptest threads done\n");
    return;
}

module_init(skiptest_init);
module_exit(skiptest_exit);
