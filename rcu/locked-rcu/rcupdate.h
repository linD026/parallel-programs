/* 
 * Read Copy Update: A global reference count of simple RCU
 * 
 * Use the memory model from C11 standard.
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

#ifndef __RCUPDATE_H__
#define __RCUPDATE_H__

#include <stdio.h>
#include <stdlib.h>

#include "../api.h"

/* For Sparse 
 */
#ifdef __CHECKER__
#define __rcu __attribute__((noderef, address_space(__rcu)));
#define rcu_check_sparse(p, space) ((void)(((typeof(*p) space *)p) == p))
#else
#define __rcu
#define rcu_check_sparse(p, space)
#endif /* __CHECKER__ */

/* RCU side */

#include <stdatomic.h>

struct rcu_node {
    void *obj;
    unsigned int count;
    struct rcu_node *next;
};

struct rcu_head {
    size_t objsize;
    struct rcu_node *node;
    struct rcu_node *current;
    spinlock_t sp;
} __attribute__((aligned(sizeof(void *))));

#define RCU_DEFINE(rcuhead)         \
    static struct rcu_head rcuhead; \
    static __thread struct rcu_node *__##rcuhead##_per_thread;

#define rcu_init(obj, head) __rcu_init((obj), (head), sizeof(*obj))

static __inline__ void __rcu_init(void *obj, struct rcu_head *head,
                                  size_t objsize)
{
    head->current = (struct rcu_node *)malloc(sizeof(struct rcu_node));
    if (!head->current) {
        fprintf(stderr, "__rcu_init:allocate failed\n");
        abort();
    }
    head->current->obj = obj;
    head->current->count = 0;
    head->current->next = NULL;
    head->objsize = objsize;
    head->node = NULL;
    spin_lock_init(&head->sp);
}

#define __RCU_READ_LOCK 0x0
#define __RCU_READ_UNLOCK 0x1
#define __RCU_READ_DEREFERENCE 0x2

static __inline__ void *__rcu_read_access(struct rcu_head *head,
                                          struct rcu_node **current, int ops)
{
    switch (ops) {
    case __RCU_READ_LOCK:
        *current = READ_ONCE(head->current);
        atomic_fetch_add_explicit(&(*current)->count, 1, memory_order_seq_cst);
        break;
    case __RCU_READ_UNLOCK:
        atomic_fetch_sub_explicit(&(*current)->count, 1, memory_order_seq_cst);
        break;
    case __RCU_READ_DEREFERENCE:
        return READ_ONCE((*current)->obj);
    }
    return NULL;
}

#define rcu_read_lock(rcu_head)                                    \
    do {                                                           \
        __rcu_read_access(&(rcu_head), &__##rcu_head##_per_thread, \
                          __RCU_READ_LOCK);                        \
    } while (0)

#define rcu_read_unlock(rcu_head)                                  \
    do {                                                           \
        __rcu_read_access(&(rcu_head), &__##rcu_head##_per_thread, \
                          __RCU_READ_UNLOCK);                      \
    } while (0)

#define rcu_dereference(rcu_head)                                  \
    ({                                                             \
        __rcu_read_access(&(rcu_head), &__##rcu_head##_per_thread, \
                          __RCU_READ_DEREFERENCE);                 \
    })

static __inline__ void rcu_assign_pointer(struct rcu_head *head, void *newval)
{
    struct rcu_node **current = &head->node;
    struct rcu_node *node = (struct rcu_node *)malloc(sizeof(struct rcu_node));
    if (!node) {
        fprintf(stderr, "rcu_assign_pointer:allocate failed\n");
        abort();
    }

    node->obj = newval;
    node->count = 0;
    node->next = NULL;

    /* Only one updater can write in */
    spin_lock(&head->sp);

    while (READ_ONCE(*current)) {
        /* we don't want to remove the same object as newval */
        if ((*current)->obj == newval) {
            spin_unlock(&head->sp);
            free(node);
            return;
        }
        current = &(*current)->next;
    }

    WRITE_ONCE(*current, head->current);
    WRITE_ONCE(head->current, node);

    /* C11 memory model */
    atomic_thread_fence(memory_order_release);

    spin_unlock(&head->sp);
}

static __inline__ void synchronize_rcu(struct rcu_head *head)
{
    struct rcu_node *want_free;

    atomic_thread_fence(memory_order_seq_cst);

    spin_lock(&head->sp);

    want_free = head->node;

    while (want_free) {
        while (READ_ONCE(want_free->count) != 0)
            barrier();

        struct rcu_node *tmp = want_free;
        want_free = want_free->next;
        free(tmp->obj);
        free(tmp);
    }

    head->node = NULL;

    atomic_thread_fence(memory_order_seq_cst);

    spin_unlock(&head->sp);
}

static __inline__ void rcu_free(struct rcu_head *head)
{
    free(head->current->obj);
    free(head->current);
}
#endif /* __RCUPDATE_H__ */
