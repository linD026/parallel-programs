/*
 * (C) 2011  Liu Bo <liubo2009@xxxxxxxxxxxxxx>
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
#ifndef _SKIPLIST_H
#define _SKIPLIST_H

#include <linux/spinlock.h>
#include <linux/stacktrace.h>

/*
 * This includes a basic skiplist implementation and builds a more
 * cache friendly variant on top meant to index ranges of keys.
 *
 * our random generation function has P at 0.5, so a rough metric
 * of good performance happens for lists up to 2^MAXLEVEL in size.
 * Since we have an array of keys, you can do 2^MAXLEVEL * SKIP_KEYS_PER_NODE
 */
#define SKIP_MAXLEVEL 32 /* skiplist_get_new_level requires <= 32 */

struct sl_node_ptr {
    struct sl_node *prev;
    struct sl_node *next;
};

/*
 * sl_node must be last in the leaf data struct.  Allocate enough
 * ram for a given size using either the sl_ptrs_size or sl_node_size
 * helpers.
 */
struct sl_node {
    int level;
    int dead;
    spinlock_t lock;
    struct sl_node_ptr ptrs[];
};

/*
 * The head list structure.  The head node has no data,
 * but it does have the full array of pointers for all the levels.
 */
struct sl_list {
    /* in the head pointer, we use head->prev to point to
	 * the highest item in the list.  But, the last item does
	 * not point back to the head.  The head->prev items
	 * are protected the by node lock on the last item
	 */
    struct sl_node *head;
    spinlock_t lock;
    unsigned int level;
};

/*
 * If you are indexing extents, embed sl_slots into your structure and use
 * sl_slot_entry to pull out your real struct.  The key and size must not
 * change if you're using rcu.
 */
struct sl_slot {
    /*
	 * when rcu is on, we use this key to verify the pointer we pull
	 * out of the array.  It must not change once the object is
	 * inserted
	 */
    unsigned long key;

    /*
	 * the range searching functions follow pointers into this slot
	 * struct and use this size field to find out how big the
	 * range is.
	 */
    unsigned long size;

    /*
	 * there is no reference count here, that's the job of whatever
	 * struct you embed this into.  Good luck.
	 */
};

/*
 * Larger values here make us faster when single threaded.  Lower values
 * increase cache misses but give more chances for concurrency.
 */
#define SKIP_KEYS_PER_NODE 32

/*
 * For indexing extents, this is a leaf in our skip list tree.
 * Each leaf has a number of pointers and the max field
 * is used to figure out the key space covered.
 */
struct sl_leaf {
    /* number of valid keys/ptrs in this leaf */
    int nr;

    /*
	 * max value of the range covered by this leaf.  This
	 * includes the size field of the very last extent,
	 * so max = keys[last_index] + ptrs[last_index]->size
	 */
    unsigned long max;

    /*
	 * sorted, all the keys
	 */
    unsigned long keys[SKIP_KEYS_PER_NODE];

    /*
	 * data pointers corresponding to the keys
	 */
    struct sl_slot *ptrs[SKIP_KEYS_PER_NODE];

    /* for freeing our objects after the grace period */
    struct rcu_head rcu_head;

    /* this needs to be at the end. The size changes based on the level */
    struct sl_node node;
};

/*
 * for a given level, how much memory we need for an array of
 * all the pointers
 */
static inline int sl_ptrs_size(int level)
{
    return sizeof(struct sl_node_ptr) * (level + 1);
}

/*
 * for a given level, how much memory we need for the
 * array of pointers and the sl_node struct
 */
static inline int sl_node_size(int level)
{
    return sizeof(struct sl_node) + sl_ptrs_size(level);
}

static inline int sl_leaf_size(int level)
{
    return sizeof(struct sl_leaf) + sl_ptrs_size(level);
}

#define sl_entry(ptr) container_of((ptr), struct sl_leaf, node)
#define sl_slot_entry(ptr, type, member) container_of(ptr, type, member)

static inline int sl_empty(const struct sl_node *head)
{
    return head->ptrs[0].next == NULL;
}

int skiplist_preload(struct sl_list *list, gfp_t gfp_mask);
int skiplist_get_new_level(struct sl_list *list, int max_level);
int skiplist_insert(struct sl_list *list, struct sl_slot *slot,
                    int preload_token);
int sl_init_list(struct sl_list *list, gfp_t mask);
struct sl_slot *skiplist_lookup(struct sl_list *list, unsigned long key,
                                unsigned long size);
struct sl_slot *skiplist_lookup_rcu(struct sl_list *list, unsigned long key,
                                    unsigned long size);
struct sl_slot *skiplist_delete(struct sl_list *list, unsigned long key,
                                unsigned long size);
int skiplist_insert_hole(struct sl_list *list, unsigned long hint,
                         unsigned long limit, unsigned long size,
                         unsigned long align, struct sl_slot *slot,
                         gfp_t gfp_mask);
void sl_lock_node(struct sl_node *n);
void sl_unlock_node(struct sl_node *n);
void sl_free_leaf(struct sl_leaf *leaf);
unsigned long sl_highest_key(struct sl_list *list);
extern void skiplist_init(void);
#endif /* _SKIPLIST_H */
