#ifndef __RCULIST_H__
#define __RCULIST_H__

#include <stddef.h>
#include "thrd_rcu.h"

/* list add, list add tail, list del, list_del_tail, for each entry
 */

#define container_of(ptr, type, member)                                        \
    __extension__({                                                            \
        const __typeof__(((type *)0)->member) *__mptr = (ptr);                 \
        (type *)((char *)__mptr - offsetof(type, member));                     \
    })

struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

static inline void list_init_rcu(struct list_head *node)
{
    node->next = node;
    barrier();
    node->prev = node;
}

static inline void __list_add_rcu(struct list_head *new, struct list_head *prev,
                                  struct list_head *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    barrier();
    rcu_assign_pointer(prev->next, new);
}

static inline void list_add_rcu(struct list_head *new, struct list_head *head)
{
    __list_add_rcu(new, head, head->next);
}

static inline void list_add_tail_rcu(struct list_head *new,
                                     struct list_head *head)
{
    __list_add_rcu(new, head->prev, head);
}

static inline void __list_del_rcu(struct list_head *prev,
                                  struct list_head *next)
{
    next->prev = prev;
    barrier();
    rcu_assign_pointer(prev->next, next);
}

static inline void list_del_rcu(struct list_head *node)
{
    __list_del_rcu(node->prev, node->next);
    list_init_rcu(node);
}

#define list_for_each(n, head) for (n = (head)->next; n != (head); n = n->next)

#define list_for_each_from(pos, head) for (; pos != (head); pos = pos->next)

#define list_for_each_safe(pos, n, head)                                \
    for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

#endif /* __RCULIST_H__ */