/*
 * thread based RCU: Partitioning reference count to per thread storage
 *
 * Provide the multiple-updater for the rcu_assign_pointer by atomic_exchange
 * Even the reference count named by rcu_nesting, it doesn't support nesting
 * locking.
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

#ifndef __THRD_RCU_H__
#define __THRD_RCU_H__

#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include "../api.h"

#ifdef __CHECKER__
#define __rcu __attribute__((noderef, address_space(__rcu)))
#define rcu_check_sparse(p, space) ((void)(((typeof(*p) space *)p) == p))
#define __force __attribute__((force))
#define rcu_uncheck(p) ((__typeof__(*p) __force *)p)
#define rcu_check(p) ((__typeof__(*p) __force __rcu *)p)
#else
#define __rcu
#define rcu_check_sparse(p, space)
#define __force
#define rcu_uncheck(p) p
#define rcu_check(p) p
#endif /* __CHECKER__ */

/* Avoid false sharing
 */
#define __rcu_aligned __attribute__((aligned(128)))

struct rcu_node {
    pthread_t tid;
    int rcu_nesting[2];
    struct rcu_node *next;
} __rcu_aligned;

struct rcu_data {
    unsigned int nr_thread;
    struct rcu_node *head;
    unsigned int rcu_thrd_nesting_idx;
    spinlock_t sp;
};

// Easly to use it.
#define __rcu_thrd_idx rcu_data.rcu_thrd_nesting_idx
#define __rcu_thrd_nesting(ptr)                                                \
    ptr->rcu_nesting[READ_ONCE(__rcu_thrd_idx) & 0x01]
#define rcu_thrd_nesting __rcu_thrd_nesting(__rcu_per_thrd_ptr)

static struct rcu_data rcu_data = { .nr_thread = 0,
                                    .head = NULL,
                                    .rcu_thrd_nesting_idx = 0,
                                    .sp = SPINLOCK_INIT };
static __thread struct rcu_node *__rcu_per_thrd_ptr;

static __inline__ struct rcu_node *__rcu_node_add(pthread_t tid)
{
    struct rcu_node **indirect = &rcu_data.head;
    struct rcu_node *node;

    node = (struct rcu_node *)malloc(sizeof(struct rcu_node));
    if (!node) {
        fprintf(stderr, "__rcu_node_add: malloc failed\n");
        abort();
    }

    node->tid = tid;
    node->rcu_nesting[0] = 0;
    node->rcu_nesting[1] = 0;
    node->next = NULL;

    spin_lock(&rcu_data.sp);

    while (*indirect) {
        if ((*indirect)->tid == node->tid) {
            spin_unlock(&rcu_data.sp);
            free(node);
            return NULL;
        }
        indirect = &(*indirect)->next;
    }

    *indirect = node;
    rcu_data.nr_thread++;

    spin_unlock(&rcu_data.sp);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    return node;
}

static __inline__ int rcu_init(void)
{
    pthread_t tid = pthread_self();

    __rcu_per_thrd_ptr = __rcu_node_add(tid);

    return (__rcu_per_thrd_ptr == NULL) ? -ENOMEM : 0;
}

static __inline__ void rcu_clean(void)
{
    struct rcu_node *node, *tmp;

    spin_lock(&rcu_data.sp);

    for (node = rcu_data.head; node != NULL; node = tmp) {
        tmp = node->next;
        if (__rcu_thrd_nesting(node) & 0x1)
            usleep(10);
        free(node);
    }

    rcu_data.head = NULL;
    rcu_data.nr_thread = 0;

    spin_unlock(&rcu_data.sp);
}

/* The per-thread reference count will only modified by their owner thread,
 * but will read by other thread. So here we use WRITE_ONCE
 */
static __inline__ void rcu_read_lock(void)
{
    WRITE_ONCE(rcu_thrd_nesting, 1);
    //__atomic_store_n(&rcu_thrd_nesting, 1, __ATOMIC_RELAXED);
}

static __inline__ void rcu_read_unlock(void)
{
    WRITE_ONCE(rcu_thrd_nesting, 0);
    //__atomic_store_n(&rcu_thrd_nesting, 0, __ATOMIC_RELAXED);
}

static __inline__ void synchronize_rcu(void)
{
    struct rcu_node *node;

    __atomic_thread_fence(__ATOMIC_SEQ_CST);

    spin_lock(&rcu_data.sp);

    /* When the rcu_thrd_nesting is odd, which mean that the lsb is being set
     * 1, that thread is in the read critical section. Also, we need to skip
     * the read side when it is in the new grace period.
     */
    for (node = rcu_data.head; node != NULL; node = node->next) {
        while (READ_ONCE(__rcu_thrd_nesting(node)) & 0x1) {
            //while (__atomic_load_n(&__rcu_thrd_nesting(node),
            //                       __ATOMIC_RELAXED) &
            //       0x1) {
            //usleep(10);
            barrier();
        }
    }

    /* Going to next grace period
     */
    __atomic_fetch_add(&__rcu_thrd_idx, 1, __ATOMIC_RELEASE);

    spin_unlock(&rcu_data.sp);

    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

#define rcu_dereference(p)                                                     \
    ({                                                                         \
        __typeof__(*p) *__r_d_p = (__typeof__(*p) __force *)READ_ONCE(p);      \
        rcu_check_sparse(p, __rcu);                                            \
        __r_d_p;                                                               \
    })

#define rcu_assign_pointer(p, v)                                               \
    ({                                                                         \
        __typeof__(*p) *__r_a_p =                                              \
                (__typeof__(*p) __force *)__atomic_exchange_n(                 \
                        &(p), (__typeof__(*(p)) __force __rcu *)v,             \
                        __ATOMIC_RELEASE);                                     \
        rcu_check_sparse(p, __rcu);                                            \
        __r_a_p;                                                               \
    })

#endif /* __THRD_RCU_H__ */
