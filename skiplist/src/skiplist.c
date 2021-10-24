/* 
 * skiplist: The sequence program of skip list implementation
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

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>

#include "skiplist.h"

#define sl_cmb() asm volatile("" : : : "memory")

struct sl_node {
    int key;
    void *val;
    struct sl_link link[0];
};

/* linked list - related function
 */

static inline void list_init(struct sl_link *node)
{
    node->next = node;
    sl_cmb();
    node->prev = node;
}

static inline void __list_add(struct sl_link *new, struct sl_link *prev,
                              struct sl_link *next)
{
    next->prev = new;
    new->next = next;
    new->prev = prev;
    sl_cmb();
    prev->next = new;
}

static inline void list_add(struct sl_link *new, struct sl_link *prev)
{
    __list_add(new, prev, prev->next);
}

static inline void __list_del(struct sl_link *prev, struct sl_link *next)
{
    next->prev = prev;
    sl_cmb();
    prev->next = next;
}

static inline void list_del(struct sl_link *node)
{
    __list_del(node->prev, node->next);
    list_init(node);
}

#define list_for_each_from(pos, head) for (; pos != head; pos = pos->next)

#define list_for_each_safe_from(pos, n, head) \
    for (n = pos->next; pos != head; pos = n, n = pos->next)

#define container_of(ptr, type, member)                        \
    __extension__({                                            \
        const __typeof__(((type *)0)->member) *__mptr = (ptr); \
        (type *)((char *)__mptr - offsetof(type, member));     \
    })

#define list_entry(ptr, i) container_of(ptr, struct sl_node, link[i])

/* the probability set at 1/2
 */

static inline void set_random(void)
{
    time_t current_time;
    srandom(time(&current_time));
}

static inline int random_level(void)
{
    int level = 0;
    uint32_t random_seed = (uint32_t)random();

    while (random_seed && (random_seed & 0x1)) {
        random_seed >>= 1;
        level++;
    }

    return level >= SL_MAXLEVEL ? SL_MAXLEVEL - 1 : level;
}

/* skip list - related function
 */

static struct sl_node *sl_node_alloc(int key, void *val, int level)
{
    struct sl_node *node;

    node =
        malloc(sizeof(struct sl_node) + (level + 1) * sizeof(struct sl_link));
    if (!node)
        return NULL;

    node->key = key;
    node->val = val;

    return node;
}

struct sl_list *sl_list_alloc(void)
{
    int i;
    struct sl_list *list = malloc(sizeof(struct sl_list));
    if (!list)
        return NULL;

    list->level = 0;
    list->size = 0;
    set_random();
    for (i = 0; i < SL_MAXLEVEL; i++)
        list_init(&list->head[i]);

    return list;
}

void sl_delete(struct sl_list *list)
{
    struct sl_link *n, *pos = list->head[0].next;

    list_for_each_safe_from(pos, n, &list->head[0])
    {
        struct sl_node *node = list_entry(pos, 0);
        free(node);
    }
    free(list);
}

void *sl_search(struct sl_list *list, int key)
{
    int i = list->level;
    struct sl_link *pos = &list->head[i];
    struct sl_link *head = &list->head[i];

    for (; i >= 0; i--) {
        pos = pos->next;
        list_for_each_from(pos, head)
        {
            struct sl_node *node = list_entry(pos, i);
            if (node->key > key)
                break;
            else if (node->key == key)
                return node->val;
        }
        pos = pos->prev;
        pos--;
        head--;
    }

    return NULL;
}

int sl_insert(struct sl_list *list, int key, void *val)
{
    int i, level = random_level();
    struct sl_link *pos = &list->head[level];
    struct sl_link *head = &list->head[level];
    struct sl_node *new = sl_node_alloc(key, val, level);

    if (!new)
        return -ENOMEM;
    if (level > list->level)
        list->level = level;

    for (i = level; i >= 0; i--) {
        pos = pos->next;
        list_for_each_from(pos, head)
        {
            struct sl_node *tmp = list_entry(pos, i);
            if (tmp->key > key) {
                break;
            } else if (tmp->key == key)
                goto failed;
        }
        pos = pos->prev;
        list_add(&new->link[i], pos);
        pos--;
        head--;
    }

    list->size++;

    return 0;
failed:
    free(new);
    return -EEXIST;
}

int sl_erase(struct sl_list *list, int key)
{
    int i = list->level;
    struct sl_link *pos = &list->head[i];
    struct sl_link *n, *head = &list->head[i];

    for (; i >= 0; i--) {
        pos = pos->next;
        list_for_each_safe_from(pos, n, head)
        {
            struct sl_node *tmp = list_entry(pos, i);
            if (tmp->key == key) {
                for (; i >= 0; i--) {
                    list_del(pos--);
                    if (list->head[i].next == &list->head[i])
                        list->level--;
                }
                free(tmp);
                list->size--;
                return 0;
            } else if (tmp->key > key)
                break;
        }
        pos = pos->prev;
        pos--;
        head--;
    }

    return -EINVAL;
}
