/* 
 * Little Read Copy Update: A simple Classic RCU
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

#ifndef __LRCU_H__
#define __LRCU_H__

#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/preempt.h>
#include <linux/cpumask.h>
#include <linux/sched.h>
#include <linux/kthread.h>

#ifdef __CHECKER__
#define __lrcu __attribute__((noderef, address_space(__lrcu)))
#define lrcu_check_sparse(p, space) ((void)(((typeof(*p) space *)p) == p))
#else
#define __lrcu
#define lrcu_check_sparse(p, space)
#endif /* __CHECKER__ */

#define NR_LRCU_PROTECTED 10

typedef void (*lrcu_callback_t)(void *);

struct lrcu_data {
    void __lrcu *list[NR_LRCU_PROTECTED];
    spinlock_t list_lock;
    lrcu_callback_t callback;
};

#define DEFINE_LRCU(name, call_back)                                        \
    struct lrcu_data name = { .list_lock =                                  \
                                      __SPIN_LOCK_UNLOCKED(name.list_lock), \
                              .callback = call_back }

static __inline__ void *__lrcu_collect_old_pointer(struct lrcu_data *lrcu_data,
                                                   void __lrcu *oldp)
{
    int i, idx = -1;

    spin_lock(&lrcu_data->list_lock);

    for (i = 0; i < NR_LRCU_PROTECTED; i++) {
        if (lrcu_data->list[i] == NULL) {
            idx = i;
            break;
        }
        barrier();
    }

    if (idx == -1) {
        pr_alert("lrcu_assign_pointer:"
                 "__lrcu_collect_old_pointer - buffer is full\n");
        return NULL;
    }

    lrcu_data->list[idx] = READ_ONCE(oldp);

    return (void __force *)lrcu_data->list[idx];
}

/* lrcu_assign_pointer() - assign to LRCU-protected pointer
 * 
 * The READ_ONCE(__l_r_rev) is to prevent the compiler optimization.
 */
#define lrcu_assign_pointer(oldp, newp, ldp)                             \
    ({                                                                   \
        uintptr_t __l_r_rev;                                             \
        lrcu_check_sparse(oldp, __lrcu);                                 \
                                                                         \
        __l_r_rev = (uintptr_t)__lrcu_collect_old_pointer(               \
                (ldp), (void __lrcu *)oldp);                             \
        if (READ_ONCE(__l_r_rev))                                        \
            smp_store_release(&oldp,                                     \
                              (typeof(*(oldp)) __force __lrcu *)(newp)); \
                                                                         \
        spin_unlock(&(ldp)->list_lock);                                  \
        (typeof(*oldp) *)__l_r_rev;                                      \
    })

/* lrcu_dereference() - dereference the LRCU-portected pointer
 *
 * This macro does not provide the lock checking.
 */
#define lrcu_dereference(p)                                      \
    ({                                                           \
        typeof(*p) *__l_r_p = (typeof(*p) *__force)READ_ONCE(p); \
        lrcu_check_sparse(p, __lrcu);                            \
        __l_r_p;                                                 \
    })

static __inline__ void lrcu_read_lock(void)
{
#ifdef CONFIG_PREEMPTION
    preempt_disable();
#endif
}

static __inline__ void lrcu_read_unlock(void)
{
#ifdef CONFIG_PREEMPTION
    preempt_enable();
#endif
}

#include <linux/kprobes.h>
static long (*lrcu_sched_setaffinity)(pid_t pid,
                                      const struct cpumask *new_mask);

static __inline__ int lrcu_sched_init(void)
{
    unsigned long (*kallsyms_lookup_name)(const char *name);
    struct kprobe kp = {
        .symbol_name = "kallsyms_lookup_name",
    };

    if (register_kprobe(&kp) < 0)
        return -1;
    kallsyms_lookup_name = (unsigned long (*)(const char *name))kp.addr;
    unregister_kprobe(&kp);
    lrcu_sched_setaffinity =
            (long (*)(pid_t, const struct cpumask *))kallsyms_lookup_name(
                    "sched_setaffinity");

    return 0;
}

#define run_on(cpu) lrcu_sched_setaffinity(current->pid, cpumask_of(cpu))

static __inline__ void synchronize_lrcu(struct lrcu_data *lrcu_data)
{
    int cpu, i;

    for_each_online_cpu(cpu) run_on(cpu);

    smp_mb();

    spin_lock(&lrcu_data->list_lock);

    for (i = 0; i < NR_LRCU_PROTECTED; i++)
        lrcu_data->list[i] = NULL;

    spin_unlock(&lrcu_data->list_lock);

    smp_mb();
}

static __inline__ int __call_lrcu(void *data)
{
    struct lrcu_data *lrcu_data = (struct lrcu_data *)data;
    int cpu, i;

    for_each_online_cpu(cpu) run_on(cpu);

    smp_mb();

    spin_lock(&lrcu_data->list_lock);

    for (i = 0; i < NR_LRCU_PROTECTED; i++) {
        if (lrcu_data->list[i] != NULL) {
            lrcu_data->callback((void __force *)lrcu_data->list[i]);
            lrcu_data->list[i] = NULL;
        }
    }

    spin_unlock(&lrcu_data->list_lock);

    smp_mb();

    do_exit(0);
}

/* TODO: Fix the .data storage problem
 * the lrcu_data only provide the .data sotrage class, it cannot pass
 * to another thread.
 * It will be like:
 * 
 * struct lrcu_data *p = kmalloc();
 * lrcu_data_init(p);
 * do_some_thing();
 * call_lrcu(p);
 * all_finish();
 * kfree(p);
 */
static __inline__ void call_lrcu(struct lrcu_data *lrcu_data)
{
    smp_mb();
    kthread_run(__call_lrcu, (void *)lrcu_data, "kthread: call_lrcu");
}

#endif /* __LRCU_H__ */
