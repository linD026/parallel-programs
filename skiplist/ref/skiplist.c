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

#include <linux/errno.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/radix-tree.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/string.h>
#include <linux/bitops.h>
#include <linux/rcupdate.h>
#include <linux/random.h>
#include <linux/lockdep.h>
#include <linux/stacktrace.h>
#include <linux/sched.h>

#include "skiplist.h"

static struct kmem_cache *slab_caches[SKIP_MAXLEVEL];

/*
 * we have preemption off anyway for insertion, make the cursor
 * per-cpu so we don't need to allocate one
 */
struct skip_preload {
    /* one of these per CPU for tracking insertion */
    struct sl_node *cursor[SKIP_MAXLEVEL + 1];

    /*
	 * the preload is filled in based on the highest possible level
	 * of the list you're preloading.  So we basically end up with
	 * one preloaded node for each max size.
	 */
    struct sl_leaf *preload[SKIP_MAXLEVEL + 1];
};

static DEFINE_PER_CPU(struct skip_preload, skip_preloads) = {
    {
        NULL,
    },
};

static void sl_init_node(struct sl_node *node, int level)
{
    spin_lock_init(&node->lock);

    node->ptrs[level].prev = NULL;
    node->level = level;
    node->dead = 0;
}

/*
 * the rcu based searches need to block reuse until a given search round
 * is done.  So, we use call_rcu for freeing the leaf structure.
 */
static void sl_free_rcu(struct rcu_head *head)
{
    struct sl_leaf *leaf = container_of(head, struct sl_leaf, rcu_head);
    kmem_cache_free(slab_caches[leaf->node.level], leaf);
}

void sl_free_leaf(struct sl_leaf *leaf)
{
    call_rcu(&leaf->rcu_head, sl_free_rcu);
}

/*
 * helper functions to wade through dead nodes pending deletion
 * and return live ones.
 */
static struct sl_node *find_live_prev(struct sl_list *list,
                                      struct sl_node *node, int level)
{
    /* head->prev points to the max, this makes sure we don't loop */
    if (node == list->head)
        return NULL;

    while (node) {
        node = rcu_dereference(node->ptrs[level].prev);
        /*
		 * the head is never dead, so we'll never walk past
		 * it down in this loop
		 */
        if (!node->dead)
            break;
    }

    return node;
}

static struct sl_node *find_live_next(struct sl_list *list,
                                      struct sl_node *node, int level)
{
    while (node) {
        node = rcu_dereference(node->ptrs[level].next);
        if (!node || !node->dead)
            break;
    }
    return node;
}

/*
 * having trouble teaching lockdep about the skiplist
 * locking.  The problem is that we're allowed to
 * hold multiple locks on the same level as long as we
 * go from left to right.
 */
void sl_lock_node(struct sl_node *n)
{
    spin_lock(&n->lock);
}

void sl_unlock_node(struct sl_node *n)
{
    if (n)
        spin_unlock(&n->lock);
}

/*
 * the cursors are used by the insertion code to remember the leaves we passed
 * on the way down to our insertion point.  Any new nodes are linking in
 * after the nodes in our cursor.
 *
 * Nodes may appear in the cursor more than once, but if so they are
 * always consecutive.  We don't have A, B, C, B, D, only
 * A, B, B, B, C, D.  When locking and unlocking things, we
 * have to make sure we leave any node inside the cursor properly locked.
 *
 * Right now, everything in the cursor must be locked.
 */
int found_in_cursor(struct sl_node **cursor, int max_level, struct sl_node *p)
{
    int i;

    for (i = 0; i <= max_level; i++) {
        if (cursor[i] == p)
            return 1;
    }
    return 0;
}

/*
 * add p into cursor at a specific level.  If p is replacing another
 * pointer, that pointer is unlocked, unless it is also at a
 * higher level in the cursor.
 *
 * p is locked unless it was already in the cursor.
 */
static void add_to_cursor(struct sl_node **cursor, int level, struct sl_node *p)
{
    struct sl_node *old;
    struct sl_node *higher;

    old = cursor[level];
    cursor[level] = p;

    if (old == p)
        return;

    if (level == SKIP_MAXLEVEL) {
        sl_lock_node(p);
        sl_unlock_node(old);
        return;
    }
    higher = cursor[level + 1];

    if (higher != p)
        sl_lock_node(p);
    if (higher != old)
        sl_unlock_node(old);
}

/*
 * same as add_to_cursor, but p must already be locked.
 */
static void add_locked_to_cursor(struct sl_node **cursor, int level,
                                 struct sl_node *p)
{
    struct sl_node *old;

    old = cursor[level];
    cursor[level] = p;

    if (old == p)
        return;

    if (level == SKIP_MAXLEVEL) {
        sl_unlock_node(old);
        return;
    }

    if (cursor[level + 1] != old)
        sl_unlock_node(old);
}

/*
 * unlock any nodes in the cursor below max_level
 */
static void free_cursor_locks(struct sl_node **cursor, int max_level)
{
    struct sl_node *p;
    int i;

    for (i = max_level; i >= 0; i--) {
        p = cursor[i];
        cursor[i] = NULL;
        if (i == 0 || cursor[i - 1] != p)
            sl_unlock_node(p);
    }
}

/*
 * helper function to link a single level during an insert.
 * prev must be locked, and it is the node we are linking after.
 *
 * This will find a live next pointer, lock it, and link it
 * with our new node
 */
static void sl_link_one_level(struct sl_list *list, struct sl_node *prev,
                              struct sl_node *node, int level)
{
    struct sl_node *next;
    struct sl_node *test;

    assert_spin_locked(&prev->lock);
    BUG_ON(prev->dead);

again:
    next = find_live_next(list, prev, level);
    if (next) {
        sl_lock_node(next);
        test = find_live_next(list, prev, level);
        if (test != next || next->dead) {
            sl_unlock_node(next);
            goto again;
        }
        /*
		 * make sure the our next and prev really point to each
		 * other now that we have next locked.
		 */
        if (find_live_prev(list, next, level) != prev) {
            sl_unlock_node(next);
            goto again;
        }
    }

    rcu_assign_pointer(node->ptrs[level].next, next);
    rcu_assign_pointer(node->ptrs[level].prev, prev);
    rcu_assign_pointer(prev->ptrs[level].next, node);

    /*
	 * if next is null, we're the last node on this level.
	 * The head->prev pointer is used to cache this fact
	 */
    if (next)
        rcu_assign_pointer(next->ptrs[level].prev, node);
    else
        rcu_assign_pointer(list->head->ptrs[level].prev, node);

    sl_unlock_node(next);
}

/*
 * link a node at a given level.  The cursor needs pointers to all the
 * nodes that are just behind us in the list.
 *
 * Link from the bottom up so that our searches from the top down don't
 * find bogus pointers
 */
static void sl_link_node(struct sl_list *list, struct sl_node *node,
                         struct sl_node **cursor, int level)
{
    int i;

    for (i = 0; i <= level; i++)
        sl_link_one_level(list, cursor[i], node, i);
}

/*
 * just like sl_link_node, but use 'after' for starting point of the link.
 * Any pointers not provided from 'after' come from the cursor.
 * 'after' must be locked.
 */
static void sl_link_after_node(struct sl_list *list, struct sl_node *node,
                               struct sl_node *after, struct sl_node **cursor,
                               int level)
{
    int i;

    /* first use all the pointers from 'after' */
    for (i = 0; i <= after->level && i <= level; i++)
        sl_link_one_level(list, after, node, i);

    /* then use the cursor for anything left */
    for (; i <= level; i++)
        sl_link_one_level(list, cursor[i], node, i);
}

/*
 * helper function to pull out the next live leaf at a given level.
 * It is not locked
 */
struct sl_leaf *sl_next_leaf(struct sl_list *list, struct sl_node *p, int l)
{
    struct sl_node *next;
    if (!p)
        return NULL;

    next = find_live_next(list, p, l);
    if (next)
        return sl_entry(next);
    return NULL;
}

/*
 * return the highest value for a given leaf.  This is cached
 * in leaf->max so that we don't have to wander into
 * the slot pointers.  The max is equal to the key + size of the
 * last slot.
 */
static unsigned long sl_max_key(struct sl_leaf *leaf)
{
    smp_rmb();
    return leaf->max;
}

/*
 * return the lowest key for a given leaf. This comes out
 * of the node key array and not the slots
 */
static unsigned long sl_min_key(struct sl_leaf *leaf)
{
    smp_rmb();
    return leaf->keys[0];
}

struct sl_leaf *sl_first_leaf(struct sl_list *list)
{
    struct sl_leaf *leaf;
    struct sl_node *p;

    p = list->head->ptrs[0].next;
    if (!p)
        return NULL;
    leaf = sl_entry(p);

    return leaf;
}

struct sl_leaf *sl_last_leaf(struct sl_list *list)
{
    struct sl_leaf *leaf;
    struct sl_node *p;

    p = list->head->ptrs[0].prev;
    if (!p)
        return NULL;
    leaf = sl_entry(p);

    return leaf;
}

/*
 * search inside the key array of a given leaf.  The leaf must be
 * locked because we're using a binary search.  This returns
 * zero if we found a slot with the key in it, and sets
 * 'slot' to the number of the slot pointer.
 *
 * 1 is returned if the key was not found, and we set slot to
 * the location where the insert needs to be performed.
 *
 */
int leaf_slot_locked(struct sl_leaf *leaf, unsigned long key,
                     unsigned long size, int *slot)
{
    int low = 0;
    int high = leaf->nr - 1;
    int mid;
    unsigned long k1;
    struct sl_slot *found;

    /*
	 * case1:
	 *       [ key ... size ]
	 *  [found .. found size  ]
	 *
	 *  case2:
	 *  [key ... size ]
	 *      [found .. found size ]
	 *
	 *  case3:
	 *  [key ...                 size ]
	 *      [ found .. found size ]
	 *
	 *  case4:
	 *  [key ...size ]
	 *         [ found ... found size ]
	 *
	 *  case5:
	 *                       [key ...size ]
	 *         [ found ... found size ]
	 */

    while (low <= high) {
        mid = low + (high - low) / 2;
        k1 = leaf->keys[mid];
        if (k1 < key) {
            low = mid + 1;
        } else if (k1 >= key + size) {
            high = mid - 1;
        } else {
            *slot = mid;
            return 0;
        }
    }

    /*
	 * nothing found, at this point we're in the slot this key would
	 * normally be inserted at.  Check the previous slot to see if
	 * it is inside the range there
	 */
    if (low > 0) {
        k1 = leaf->keys[low - 1];
        found = leaf->ptrs[low - 1];

        /* case1, case2, case5 */
        if (k1 < key + size && k1 + found->size > key) {
            *slot = low - 1;
            return 0;
        }

        /* case3, case4 */
        if (k1 < key + size && k1 >= key) {
            *slot = low - 1;
            return 0;
        }
    }
    *slot = low;
    return 1;
}

/*
 * sequential search for lockless rcu.  The insert/deletion routines
 * try to order their operations to make this safe.  See leaf_slot_locked
 * for a list of extent range cases we're trying to cover.
 */
static int leaf_slot(struct sl_leaf *leaf, unsigned long key,
                     unsigned long size, int *slot)
{
    int i;
    int cur;
    int last;
    unsigned long this_key;
    struct sl_slot *found;

again:
    cur = 0;
    last = leaf->nr;

    /* find the first slot greater than our key */
    for (i = 0; i < last; i++) {
        smp_rmb();
        this_key = leaf->keys[i];
        if (this_key >= key + size)
            break;
        cur = i;
    }
    if (leaf->keys[cur] < key + size) {
        /*
		 * if we're in the middle of an insert, pointer may
		 * be null.  This little loop will wait for the insertion
		 * to finish.
		 */
        while (1) {
            found = rcu_dereference(leaf->ptrs[cur]);
            if (found)
                break;
            cpu_relax();
        }

        /* insert is juggling our slots, try again */
        if (found->key != leaf->keys[cur])
            goto again;

        /* case1, case2, case5 */
        if (found->key < key + size && found->key + found->size > key) {
            *slot = cur;
            return 0;
        }

        /* case3, case4 */
        if (found->key < key + size && found->key >= key) {
            *slot = cur;
            return 0;
        }

        *slot = cur + 1;
        return 1;
    }
    *slot = cur;
    return 1;
}

/*
 * this does the dirty work of splitting and/or shifting a leaf
 * to get a new slot inside.  The leaf must be locked.  slot
 * tells us where into we should insert in the leaf and the cursor
 * should have all the pointers we need to fully link any new nodes
 * we have to create.  leaf and everything in the cursor must be locked.
 */
static noinline int add_key_to_leaf(struct sl_list *list, struct sl_leaf *leaf,
                                    struct sl_slot *slot_ptr, unsigned long key,
                                    int slot, struct sl_node **cursor,
                                    int preload_token)
{
    struct sl_leaf *split;
    struct skip_preload *skp;
    int level;

    /* no splitting required, just shift our way in */
    if (leaf->nr < SKIP_KEYS_PER_NODE)
        goto insert;

    skp = &get_cpu_var(skip_preloads);
    split = skp->preload[preload_token];

    /*
	 * we need to insert a new leaf, but we try not to insert a new leaf at
	 * the same height as our previous one, it's just a waste of high level
	 * searching.  If the new node is the same level or lower than the
	 * existing one, try to use a level 0 leaf instead.
	 *
	 * The preallocation code tries to keep a level 0 leaf preallocated,
	 * lets see if we can grab one.
	 */
    if (leaf->node.level > 0 && split->node.level <= leaf->node.level) {
        if (skp->preload[0]) {
            preload_token = 0;
            split = skp->preload[0];
        }
    }
    skp->preload[preload_token] = NULL;

    level = split->node.level;

    /*
	 * bump our list->level to whatever we've found.  Nobody allocating
	 * a new node is going to set it higher than list->level + 1
	 */
    if (level > list->level)
        list->level = level;

    /*
	 * this locking is really only required for the small window where
	 * we are linking the node and someone might be deleting one of the
	 * nodes we are linking with.  The leaf passed in was already
	 * locked.
	 */
    sl_lock_node(&split->node);

    if (slot == leaf->nr) {
        /*
		 * our new slot just goes at the front of the new leaf, don't
		 * bother shifting things in from the previous leaf.
		 */
        slot = 0;
        split->nr = 1;
        split->max = key + slot_ptr->size;
        split->keys[0] = key;
        split->ptrs[0] = slot_ptr;
        smp_wmb();
        sl_link_after_node(list, &split->node, &leaf->node, cursor, level);
        sl_unlock_node(&split->node);
        return 0;
    } else {
        int nr = SKIP_KEYS_PER_NODE / 2;
        int mid = SKIP_KEYS_PER_NODE - nr;
        int src_i = mid;
        int dst_i = 0;
        int orig_nr = leaf->nr;

        /* split the previous leaf in half and copy items over */
        split->nr = nr;
        split->max = leaf->max;

        while (src_i < slot) {
            split->keys[dst_i] = leaf->keys[src_i];
            split->ptrs[dst_i++] = leaf->ptrs[src_i++];
        }

        if (slot >= mid) {
            split->keys[dst_i] = key;
            split->ptrs[dst_i++] = slot_ptr;
            split->nr++;
        }

        while (src_i < orig_nr) {
            split->keys[dst_i] = leaf->keys[src_i];
            split->ptrs[dst_i++] = leaf->ptrs[src_i++];
        }

        sl_link_after_node(list, &split->node, &leaf->node, cursor, level);
        nr = SKIP_KEYS_PER_NODE - nr;

        /*
		 * now what we have all the items copied and our new
		 * leaf inserted, update the nr in this leaf.  Anyone
		 * searching in rculand will find the fully updated
		 */
        leaf->max = leaf->keys[nr - 1] + leaf->ptrs[nr - 1]->size;
        smp_wmb();
        leaf->nr = nr;
        sl_unlock_node(&split->node);

        /*
		 * if the slot was in split item, we're done,
		 * otherwise we need to move down into the
		 * code below that shifts the items and
		 * inserts the new key
		 */
        if (slot >= mid)
            return 0;
    }
insert:
    if (slot < leaf->nr) {
        int i;

        /*
		 * put something sane into the new last slot so rcu
		 * searchers won't get confused
		 */
        leaf->keys[leaf->nr] = 0;
        leaf->ptrs[leaf->nr] = NULL;
        smp_wmb();

        /* then bump the nr */
        leaf->nr++;

        /*
		 * now step through each pointer after our
		 * destination and bubble it forward.  memcpy
		 * would be faster but rcu searchers will be
		 * able to validate pointers as they go with
		 * this method.
		 */
        for (i = leaf->nr - 1; i > slot; i--) {
            leaf->keys[i] = leaf->keys[i - 1];
            leaf->ptrs[i] = leaf->ptrs[i - 1];
            /*
			 * make sure the key/pointer pair is
			 * fully visible in the new home before
			 * we move forward
			 */
            smp_wmb();
        }

        /* finally stuff in our key */
        leaf->keys[slot] = key;
        leaf->ptrs[slot] = slot_ptr;
        smp_wmb();
    } else {
        /*
		 * just extending the leaf, toss
		 * our key in and update things
		 */
        leaf->max = key + slot_ptr->size;
        leaf->keys[slot] = key;
        leaf->ptrs[slot] = slot_ptr;

        smp_wmb();
        leaf->nr++;
    }
    return 0;
}

/*
 * when we're extending a leaf with a new key, make sure the
 * range of the [key,size] doesn't extend into the next
 * leaf.
 */
static int check_overlap(struct sl_list *list, struct sl_leaf *leaf,
                         unsigned long key, unsigned long size)
{
    struct sl_node *p;
    struct sl_leaf *next;
    int ret = 0;

    p = leaf->node.ptrs[0].next;
    if (!p)
        return 0;

    sl_lock_node(p);
    next = sl_entry(p);
    if (key + size > sl_min_key(next))
        ret = 1;
    sl_unlock_node(p);

    return ret;
}

/*
 * helper function for insert.  This will either return an existing
 * key or insert a new slot into the list.  leaf must be locked,
 * and everything in the cursor must be locked.
 */
static noinline int find_or_add_key(struct sl_list *list, unsigned long key,
                                    unsigned long size, struct sl_leaf *leaf,
                                    struct sl_slot *slot_ptr,
                                    struct sl_node **cursor, int preload_token)
{
    int ret;
    int slot;

    if (check_overlap(list, leaf, key, size)) {
        ret = -EEXIST;
        goto out;
    }
    if (key < leaf->max) {
        ret = leaf_slot_locked(leaf, key, size, &slot);
        if (ret == 0) {
            ret = -EEXIST;
            goto out;
        }
    } else {
        slot = leaf->nr;
    }

    add_key_to_leaf(list, leaf, slot_ptr, key, slot, cursor, preload_token);
    ret = 0;

out:
    return ret;
}

/*
 * pull a new leaf out of the prealloc area, and insert the slot/key into it
 */
static struct sl_leaf *alloc_leaf(struct sl_slot *slot_ptr, unsigned long key,
                                  int preload_token)
{
    struct sl_leaf *leaf;
    struct skip_preload *skp;
    int level;

    skp = &get_cpu_var(skip_preloads);
    leaf = skp->preload[preload_token];
    skp->preload[preload_token] = NULL;
    level = leaf->node.level;

    leaf->keys[0] = key;
    leaf->ptrs[0] = slot_ptr;
    leaf->nr = 1;
    leaf->max = key + slot_ptr->size;
    return leaf;
}

/*
 * helper to grab the cursor from the prealloc area
 * you must already have preempt off.  The whole
 * cursor is zero'd out, so don't call this if you're
 * currently using the cursor.
 */
static struct sl_node **get_cursor(void)
{
    struct skip_preload *skp;
    skp = &get_cpu_var(skip_preloads);
    memset(skp->cursor, 0, sizeof(skp->cursor[0]) * (SKIP_MAXLEVEL + 1));
    return skp->cursor;
}

/*
 * this returns with preempt disabled and the preallocation
 * area setup for a new insert.  To get there, it may or
 * may not allocate a new leaf for the next insert.
 *
 * If allocations are done, this will also try to preallocate a level 0
 * leaf, which allows us to optimize insertion by not placing two
 * adjacent nodes together with the same level.
 *
 * This returns < 0 on errors.  If everything works, it returns a preload
 * token which you should use when fetching your preallocated items.
 *
 * The token allows us to preallocate based on the current
 * highest level of the list.  For a list of level N, we won't allocate
 * higher than N + 1.
 */
int skiplist_preload(struct sl_list *list, gfp_t gfp_mask)
{
    struct skip_preload *skp;
    struct sl_leaf *leaf;
    struct sl_leaf *leaf0 = NULL;
    int alloc_leaf0 = 1;
    int level;
    int max_level = min_t(int, list->level + 1, SKIP_MAXLEVEL - 1);
    int token = max_level;

    preempt_disable();
    skp = &get_cpu_var(skip_preloads);
    if (max_level && !skp->preload[0])
        alloc_leaf0 = 1;

    if (skp->preload[max_level])
        return token;

    preempt_enable();
    level = skiplist_get_new_level(list, max_level);
    leaf = kmem_cache_alloc(slab_caches[level], gfp_mask);
    if (leaf == NULL)
        return -ENOMEM;

    if (alloc_leaf0)
        leaf0 = kmem_cache_alloc(slab_caches[0], gfp_mask);

    preempt_disable();
    skp = &get_cpu_var(skip_preloads);

    if (leaf0) {
        if (skp->preload[0] == NULL) {
            sl_init_node(&leaf0->node, 0);
            skp->preload[0] = leaf0;
        } else {
            kmem_cache_free(slab_caches[0], leaf0);
        }
    }

    if (skp->preload[max_level]) {
        kmem_cache_free(slab_caches[level], leaf);
        return token;
    }
    sl_init_node(&leaf->node, level);
    skp->preload[max_level] = leaf;

    return token;
}
EXPORT_SYMBOL(skiplist_preload);

/*
 * use the kernel prandom call to pick a new random level.  This
 * uses P = .50.  If you bump the SKIP_MAXLEVEL past 32 bits,
 * this function needs updating.
 */
int skiplist_get_new_level(struct sl_list *list, int max_level)
{
    int level = 0;
    unsigned long randseed;

    randseed = prandom_u32();

    while (randseed && (randseed & 1)) {
        randseed >>= 1;
        level++;
        if (level == max_level)
            break;
    }
    return (level >= SKIP_MAXLEVEL ? SKIP_MAXLEVEL - 1 : level);
}
EXPORT_SYMBOL(skiplist_get_new_level);

/*
 * just return the level of the leaf we're going to use
 * for the next insert
 */
static int pending_insert_level(int preload_token)
{
    struct skip_preload *skp;
    skp = &get_cpu_var(skip_preloads);
    return skp->preload[preload_token]->node.level;
}

/*
 * after a lockless search, this makes sure a given key is still
 * inside the min/max of a leaf.  If not, you have to repeat the
 * search and try again.
 */
static int verify_key_in_leaf(struct sl_leaf *leaf, unsigned long key,
                              unsigned long size)
{
    if (leaf->node.dead)
        return 0;

    if (key + size < sl_min_key(leaf) || key >= sl_max_key(leaf))
        return 0;
    return 1;
}

/* The insertion code tries to delay taking locks for as long as possible.
 * Once we've found a good place to insert, we need to make sure the leaf
 * we have picked is still a valid location.
 *
 * This checks the previous and next pointers to make sure everything is
 * still correct for the insert.  You should send an unlocked leaf, and
 * it will return 1 with the leaf locked if everything worked.
 *
 * We return 0 if the insert cannot proceed, and the leaf is returned unlocked.
 */
static int verify_key_in_path(struct sl_list *list, struct sl_node *node,
                              unsigned long key, unsigned long size, int level,
                              struct sl_node **cursor, struct sl_node **locked)
{
    struct sl_leaf *prev = NULL;
    struct sl_leaf *next;
    struct sl_node *p;
    struct sl_leaf *leaf = NULL;
    struct sl_node *lock1;
    struct sl_node *lock2;
    struct sl_node *lock3;

    BUG_ON(*locked);

again:
    lock1 = NULL;
    lock2 = NULL;
    lock3 = NULL;
    if (node != list->head) {
        p = node->ptrs[level].prev;
        if (!found_in_cursor(cursor, SKIP_MAXLEVEL, p)) {
            lock1 = p;
            sl_lock_node(p);
        }
        sl_lock_node(node);
        lock2 = node;

        if (p->dead || node->dead)
            goto out;

        if (p != list->head)
            prev = sl_entry(p);

        /*
		 * once we have the locks, make sure everyone
		 * still points to each other
		 */
        if (node->ptrs[level].prev != p || p->ptrs[level].next != node) {
            sl_unlock_node(lock1);
            sl_unlock_node(lock2);
            goto again;
        }

        leaf = sl_entry(node);
    } else {
        sl_lock_node(node);
        lock2 = node;
    }

    /*
	 * rule #1, the key must be greater than the max of the previous
	 * leaf
	 */
    if (prev && key < sl_max_key(prev))
        goto out;

    /* we're done with prev, unlock it */
    sl_unlock_node(lock1);
    lock1 = NULL;

    p = node->ptrs[level].next;
    if (p)
        next = sl_entry(p);
    else
        next = NULL;

    if (next) {
        sl_lock_node(&next->node);
        lock3 = &next->node;
        /*
		 * rule #2 the key must be smaller than the min key
		 * in the next node
		 */
        if (node->ptrs[level].next != &next->node ||
            next->node.ptrs[level].prev != node || next->node.dead ||
            key >= sl_min_key(next)) {
            /* next may be in the middle of a delete
			 * here.  If so, we can't just goto
			 * again because the delete is waiting
			 * for our lock on node.
			 * FIXME, we can try harder to avoid
			 * the goto out here
			 */
            goto out;
        }
    }
    /*
	 * return with our leaf locked and sure that our leaf is the
	 * best place for this key
	 */
    *locked = node;
    sl_unlock_node(lock1);
    sl_unlock_node(lock3);
    return 1;

out:
    sl_unlock_node(lock1);
    sl_unlock_node(lock2);
    sl_unlock_node(lock3);
    return 0;
}

/*
 * Before calling this you must have stocked the preload area by
 * calling skiplist_preload, and you must have kept preemption
 * off.  preload_token comes from skiplist_preload, pass in
 * exactly what preload gave you.
 *
 * More details in the comments below.
 */
int skiplist_insert(struct sl_list *list, struct sl_slot *slot,
                    int preload_token)
{
    struct sl_node **cursor;
    struct sl_node *p;
    struct sl_node *ins_locked = NULL;
    struct sl_leaf *leaf;
    unsigned long key = slot->key;
    unsigned long size = slot->size;
    unsigned long min_key;
    unsigned long max_key;
    int level;
    int pending_level = pending_insert_level(preload_token);
    int ret;

    rcu_read_lock();
    ret = -EEXIST;
    cursor = get_cursor();

    /*
	 * notes on pending_level, locking and general flow:
	 *
	 * pending_level is the level of the node we might insert
	 * if we can't find free space in the tree.  It's important
	 * because it tells us where our cursor is going to start
	 * recording nodes, and also the first node we have to lock
	 * to keep other inserts from messing with the nodes in our cursor.
	 *
	 * The most common answer is pending_level == 0, which is great
	 * because that means we won't have to take a lock until the
	 * very last level.
	 *
	 * if we're really lucky, we doing a level 0 insert for a key past
	 * our current max.  We can just jump down to the insertion
	 * code
	 */
    leaf = sl_last_leaf(list);
    if (leaf && sl_min_key(leaf) <= key &&
        (pending_level == 0 || leaf->nr < SKIP_KEYS_PER_NODE)) {
        p = &leaf->node;

        /* lock and recheck */
        sl_lock_node(p);

        if (!p->dead && sl_min_key(leaf) <= key && leaf == sl_last_leaf(list) &&
            leaf->node.ptrs[0].next == NULL &&
            (pending_level == 0 || leaf->nr < SKIP_KEYS_PER_NODE)) {
            ins_locked = p;
            level = 0;
            goto find_or_add;
        } else {
            sl_unlock_node(p);
        }
    }

again:
    /*
	 * the goto again code below will bump pending level
	 * so we start locking higher and higher in the tree as
	 * contention increases.  Make sure to limit
	 * it to SKIP_MAXLEVEL
	 */
    pending_level = min(pending_level, SKIP_MAXLEVEL);
    p = list->head;
    level = list->level;

    /*
	 * once we hit pending_level, we have to start filling
	 * in the cursor and locking nodes
	 */
    if (level <= pending_level) {
        if (level != pending_level)
            add_to_cursor(cursor, pending_level, p);
        add_to_cursor(cursor, level, p);
    }

    /*
	 * skip over any NULL levels in the list
	 */
    while (p->ptrs[level].next == NULL && level > 0) {
        level--;
        if (level <= pending_level) {
            add_to_cursor(cursor, level, p);
        }
    }

    do {
        while (1) {
            leaf = sl_next_leaf(list, p, level);
            if (!leaf) {
                /*
				 * if we're at level 0 and p points to
				 * the head, the list is just empty.  If
				 * we're not at level 0 yet, keep walking
				 * down.
				 */
                if (p == list->head || level != 0)
                    break;

                /*
				 * p was the last leaf on the bottom level,
				 * We're here because 'key' was bigger than the
				 * max key in p.  find_or_add will append into
				 * the last leaf.
				 */
                goto find_or_add;
            }

            /*
			 * once we get down to the pending  level, we have to
			 * start locking.  Lock the node and verify it really
			 * is exactly the node we expected to find.  It may
			 * get used in the cursor.
			 */
            if (level <= pending_level) {
                sl_lock_node(&leaf->node);
                if (leaf->node.dead ||
                    find_live_next(list, p, level) != &leaf->node ||
                    find_live_prev(list, &leaf->node, level) != p) {
                    sl_unlock_node(&leaf->node);
                    if (!found_in_cursor(cursor, pending_level, p))
                        sl_unlock_node(p);
                    free_cursor_locks(cursor, pending_level);
                    goto again;
                }
            }
            min_key = sl_min_key(leaf);
            max_key = sl_max_key(leaf);

            /*
			 * strictly speaking this test is covered again below.
			 * But usually we have to walk forward through the
			 * pointers, so this is the most common condition.  Try
			 * it first.
			 */
            if (key >= max_key)
                goto next;

            if (key < min_key) {
                /*
				 * when we aren't locking, we have to make sure
				 * new nodes haven't appeared between p and us.
				 */
                if (level > pending_level &&
                    (find_live_prev(list, &leaf->node, level) != p ||
                     min_key != sl_min_key(leaf))) {
                    goto again;
                }

                /*
				 * our key is smaller than the smallest key in
				 * leaf.  If we're not in level 0 yet, we don't
				 * want to cross over into the leaf
				 */
                if (level != 0) {
                    if (level <= pending_level)
                        sl_unlock_node(&leaf->node);
                    break;
                }

                /*
				 * we are in level 0, just stuff our slot into
				 * the front of this leaf.  We could also stuff
				 * our slot into p. FIXME, we should pick the
				 * leaf with the smallest number of items.
				 */
                if (level <= pending_level &&
                    !found_in_cursor(cursor, pending_level, p)) {
                    sl_unlock_node(p);
                }

                p = &leaf->node;
                goto find_or_add;
            }

            if (key < sl_max_key(leaf)) {
                /*
				 * our key is >= the min and < the max.  This
				 * leaf is the one true home for our key.  The
				 * level doesn't matter, we could walk  the
				 * whole tree and this would still be the best
				 * location.
				 *
				 * So, stop now and do the insert.  Our cursor
				 * might not be fully formed down to level0,
				 * but that's ok because every pointer from
				 * our current level down to zero is going to
				 * be this one leaf.  find_or_add deals with
				 * all of that.
				 *
				 * If we haven't already locked this leaf,
				 * do so now and make sure it still is
				 * the right location for our key.
				 */
                if (level > pending_level) {
                    sl_lock_node(&leaf->node);
                    if (key < sl_min_key(leaf) || key >= sl_max_key(leaf)) {
                        sl_unlock_node(&leaf->node);
                        pending_level = level;
                        goto again;
                    }
                    /*
					 * remember that we've locked this
					 * leaf for the goto find_or_add
					 */
                    ins_locked = &leaf->node;
                }

                /* unless p is in our cursor, we're done with it */
                if (level <= pending_level &&
                    !found_in_cursor(cursor, pending_level, p)) {
                    sl_unlock_node(p);
                }

                p = &leaf->node;
                goto find_or_add;
            }
        next:
            /* walk our lock forward */
            if (level <= pending_level &&
                !found_in_cursor(cursor, pending_level, p)) {
                sl_unlock_node(p);
            }
            p = &leaf->node;
        }

        /*
		 * the while loop is done with this level.  Put
		 * p into our cursor if we've started locking/
		 */
        if (level <= pending_level)
            add_locked_to_cursor(cursor, level, p);

        level--;

        /*
		 * pending_level is the line that tells us where we
		 * need to start locking.  Each node
		 * we record in the cursor needs to be exactly right,
		 * so we verify the first node in the cursor here.
		 * At this point p isn't in the cursor yet but it
		 * will be (or a downstream pointer at the
		 * same level).
		 */
        if (level == pending_level) {
            struct sl_node *locked = NULL;
            if (!verify_key_in_path(list, p, key, size, level + 1, cursor,
                                    &locked)) {
                pending_level++;
                goto again;
            }
            cursor[level] = locked;
        }
    } while (level >= 0);

    /* we only get here if the list is completely empty.  FIXME
	 * this can be folded into the find_or_add code below
	 */
    if (!cursor[0]) {
        add_to_cursor(cursor, 0, list->head);
        if (list->head->ptrs[0].next != NULL) {
            free_cursor_locks(cursor, pending_level);
            goto again;
        }
    }
    leaf = alloc_leaf(slot, key, preload_token);
    level = leaf->node.level;

    if (level > list->level) {
        list->level++;
        cursor[list->level] = list->head;
    }

    sl_link_node(list, &leaf->node, cursor, level);
    ret = 0;
    free_cursor_locks(cursor, list->level);
    rcu_read_unlock();

    return ret;

find_or_add:

    leaf = sl_entry(p);
    if (!ins_locked) {
        if (level > pending_level) {
            /* our cursor is empty, lock this one node */
            if (!verify_key_in_path(list, p, key, size, 0, cursor,
                                    &ins_locked)) {
                free_cursor_locks(cursor, pending_level);
                pending_level++;
                goto again;
            }
        } else if (!found_in_cursor(cursor, pending_level, p)) {
            /* we have a good cursor, but we're linking after
			 * p.  Make sure it gets unlocked below
			 */
            ins_locked = p;
        }
    }

    ret = find_or_add_key(list, key, size, leaf, slot, cursor, preload_token);
    free_cursor_locks(cursor, pending_level);
    sl_unlock_node(ins_locked);
    rcu_read_unlock();
    return ret;
}
EXPORT_SYMBOL(skiplist_insert);

/*
 * lookup has two stages.  First we find the leaf that should have
 * our key, and then we go through all the slots in that leaf and
 * look for the key.  This helper function is just the first stage
 * and it must be called under rcu_read_lock().  You may be using the
 * non-rcu final lookup variant, but this part must be rcu.
 *
 * We'll return NULL if we find nothing or the candidate leaf
 * for you to search.
 */
static struct sl_leaf *skiplist_lookup_leaf(struct sl_list *list,
                                            struct sl_node **last,
                                            unsigned long key,
                                            unsigned long size)
{
    struct sl_node *p;
    struct sl_leaf *leaf;
    int level;
    struct sl_leaf *leaf_ret = NULL;
    unsigned long max_key = 0;
    unsigned long min_key = 0;

again:
    level = list->level;
    p = list->head;
    do {
        while ((leaf = sl_next_leaf(list, p, level))) {
            max_key = sl_max_key(leaf);
            min_key = sl_min_key(leaf);

            if (key >= max_key)
                goto next;

            if (key < min_key) {
                smp_rmb();

                /*
				 * we're about to stop walking.  Make sure
				 * no new pointers have been inserted between
				 * p and our leaf
				 */
                if (find_live_prev(list, &leaf->node, level) != p ||
                    sl_min_key(leaf) != min_key || p->dead || leaf->node.dead) {
                    goto again;
                }
                break;
            }

            if (key < max_key) {
                leaf_ret = leaf;
                goto done;
            }
        next:
            p = &leaf->node;
        }
        level--;
    } while (level >= 0);

done:
    if (last)
        *last = p;
    return leaf_ret;
}

/*
 * this lookup function expects RCU to protect the slots in the leaves
 * as well as the skiplist indexing structures
 *
 * Note, you must call this with rcu_read_lock held, and you must verify
 * the result yourself.  If the key field of the returned slot doesn't
 * match your key, repeat the lookup.  Reference counting etc is also
 * all your responsibility.
 */
struct sl_slot *skiplist_lookup_rcu(struct sl_list *list, unsigned long key,
                                    unsigned long size)
{
    struct sl_leaf *leaf;
    struct sl_slot *slot_ret = NULL;
    int slot;
    int ret;

again:
    leaf = skiplist_lookup_leaf(list, NULL, key, size);
    if (leaf) {
        ret = leaf_slot(leaf, key, size, &slot);
        if (ret == 0)
            slot_ret = rcu_dereference(leaf->ptrs[slot]);
        else if (!verify_key_in_leaf(leaf, key, size))
            goto again;
    }
    return slot_ret;
}
EXPORT_SYMBOL(skiplist_lookup_rcu);

/*
 * this lookup function only uses RCU to protect the skiplist indexing
 * structs.  The actual slots are protected by full locks.
 */
struct sl_slot *skiplist_lookup(struct sl_list *list, unsigned long key,
                                unsigned long size)
{
    struct sl_leaf *leaf;
    struct sl_slot *slot_ret = NULL;
    struct sl_node *p;
    int slot;
    int ret;

again:
    rcu_read_lock();
    leaf = skiplist_lookup_leaf(list, &p, key, size);
    if (leaf) {
        sl_lock_node(&leaf->node);
        if (!verify_key_in_leaf(leaf, key, size)) {
            sl_unlock_node(&leaf->node);
            rcu_read_unlock();
            goto again;
        }
        ret = leaf_slot_locked(leaf, key, size, &slot);
        if (ret == 0)
            slot_ret = leaf->ptrs[slot];
        sl_unlock_node(&leaf->node);
    }
    rcu_read_unlock();
    return slot_ret;
}
EXPORT_SYMBOL(skiplist_lookup);

/* helper for skiplist_insert_hole.  the iommu requires alignment */
static unsigned long align_start(unsigned long val, unsigned long align)
{
    return (val + align - 1) & ~(align - 1);
}

/*
 * this is pretty ugly, but it is used to find a free spot in the
 * tree for a new iommu allocation.  We start from a given
 * hint and try to find an aligned range of a given size.
 *
 * Send the slot pointer, and we'll update it with the location
 * we found.
 *
 * This will return -EAGAIN if we found a good spot but someone
 * raced in and allocated it before we could.  This gives the
 * caller the chance to update their hint.
 *
 * This will return -EEXIST if we couldn't find anything at all
 *
 * returns 0 if all went well, or some other negative error
 * if things went badly.
 */
int skiplist_insert_hole(struct sl_list *list, unsigned long hint,
                         unsigned long limit, unsigned long size,
                         unsigned long align, struct sl_slot *slot,
                         gfp_t gfp_mask)
{
    unsigned long last_end = 0;
    struct sl_node *p;
    struct sl_leaf *leaf;
    int i;
    int ret = -EEXIST;
    int preload_token;
    int pending_level;

    preload_token = skiplist_preload(list, gfp_mask);
    if (preload_token < 0) {
        return preload_token;
    }
    pending_level = pending_insert_level(preload_token);

    /* step one, lets find our hint */
    rcu_read_lock();
again:

    last_end = max(last_end, hint);
    last_end = align_start(last_end, align);
    slot->key = align_start(hint, align);
    slot->size = size;
    leaf = skiplist_lookup_leaf(list, &p, hint, 1);
    if (!p)
        p = list->head;

    if (leaf && !verify_key_in_leaf(leaf, hint, size)) {
        goto again;
    }

again_lock:
    sl_lock_node(p);
    if (p->dead) {
        sl_unlock_node(p);
        goto again;
    }

    if (p != list->head) {
        leaf = sl_entry(p);
        /*
		 * the leaf we found was past the hint,
		 * go back one
		 */
        if (sl_max_key(leaf) > hint) {
            struct sl_node *locked = p;
            p = p->ptrs[0].prev;
            sl_unlock_node(locked);
            goto again_lock;
        }
        last_end = align_start(sl_max_key(sl_entry(p)), align);
    }

    /*
	 * now walk at level 0 and find a hole.  We could use lockless walks
	 * if we wanted to bang more on the insertion code, but this
	 * instead holds the lock on each node as we inspect it
	 *
	 * This is a little sloppy, insert will return -eexist if we get it
	 * wrong.
	 */
    while (1) {
        leaf = sl_next_leaf(list, p, 0);
        if (!leaf)
            break;

        /* p and leaf are locked */
        sl_lock_node(&leaf->node);
        if (last_end > sl_max_key(leaf))
            goto next;

        for (i = 0; i < leaf->nr; i++) {
            if (last_end > leaf->keys[i])
                continue;
            if (leaf->keys[i] - last_end >= size) {
                if (last_end + size > limit) {
                    sl_unlock_node(&leaf->node);
                    goto out_rcu;
                }

                sl_unlock_node(p);
                slot->key = last_end;
                slot->size = size;
                goto try_insert;
            }
            last_end = leaf->keys[i] + leaf->ptrs[i]->size;
            last_end = align_start(last_end, align);
            if (last_end + size > limit) {
                sl_unlock_node(&leaf->node);
                goto out_rcu;
            }
        }
    next:
        sl_unlock_node(p);
        p = &leaf->node;
    }

    if (last_end + size <= limit) {
        sl_unlock_node(p);
        slot->key = last_end;
        slot->size = size;
        goto try_insert;
    }

out_rcu:
    /* we've failed */
    sl_unlock_node(p);
    rcu_read_unlock();
    preempt_enable();

    return ret;

try_insert:
    /*
	 * if the pending_level is zero or there is room in the
	 * leaf, we're ready to insert.  This is true most of the
	 * time, and we won't have to drop our lock and give others
	 * the chance to race in and steal our spot.
	 */
    if (leaf && (pending_level == 0 || leaf->nr < SKIP_KEYS_PER_NODE) &&
        !leaf->node.dead &&
        (slot->key >= sl_min_key(leaf) &&
         slot->key + slot->size <= sl_max_key(leaf))) {
        /* pass null for a cursor, it won't get used */
        ret = find_or_add_key(list, slot->key, size, leaf, slot, NULL,
                              preload_token);
        sl_unlock_node(&leaf->node);
        rcu_read_unlock();
        goto out;
    }
    /*
	 * no such luck, drop our lock and try the insert the
	 * old fashioned way
	 */
    if (leaf)
        sl_unlock_node(&leaf->node);

    rcu_read_unlock();
    ret = skiplist_insert(list, slot, preload_token);

out:
    /*
	 * if we get an EEXIST here, it just means we lost the race.
	 * return eagain to the caller so they can update the hint
	 */
    if (ret == -EEXIST)
        ret = -EAGAIN;

    preempt_enable();
    return ret;
}
EXPORT_SYMBOL(skiplist_insert_hole);

/*
 * we erase one level at a time, from top to bottom.
 * The basic idea is to find a live prev and next pair,
 * and make them point to each other.
 *
 * For a given level, this takes locks on prev, node, next
 * makes sure they all point to each other and then
 * removes node from the middle.
 *
 * The node must already be marked dead and it must already
 * be empty.
 */
static void erase_one_level(struct sl_list *list, struct sl_node *node,
                            int level)
{
    struct sl_node *prev;
    struct sl_node *next;
    struct sl_node *test_prev;
    struct sl_node *test_next;

again:
    prev = find_live_prev(list, node, level);
    sl_lock_node(prev);
    sl_lock_node(node);

    test_prev = find_live_prev(list, node, level);
    if (test_prev != prev || prev->dead) {
        sl_unlock_node(prev);
        sl_unlock_node(node);
        goto again;
    }

again_next:
    next = find_live_next(list, prev, level);
    if (next) {
        sl_lock_node(next);
        test_next = find_live_next(list, prev, level);
        if (test_next != next || next->dead) {
            sl_unlock_node(next);
            goto again_next;
        }
        test_prev = find_live_prev(list, next, level);
        test_next = find_live_next(list, prev, level);
        if (test_prev != prev || test_next != next) {
            sl_unlock_node(prev);
            sl_unlock_node(node);
            sl_unlock_node(next);
            goto again;
        }
    } else {
        test_next = find_live_next(list, prev, level);
        if (test_next) {
            sl_unlock_node(prev);
            sl_unlock_node(node);
            goto again;
        }
    }
    rcu_assign_pointer(prev->ptrs[level].next, next);
    if (next)
        rcu_assign_pointer(next->ptrs[level].prev, prev);
    else if (prev != list->head)
        rcu_assign_pointer(list->head->ptrs[level].prev, prev);
    else
        rcu_assign_pointer(list->head->ptrs[level].prev, NULL);

    sl_unlock_node(prev);
    sl_unlock_node(node);
    sl_unlock_node(next);
}

void sl_erase(struct sl_list *list, struct sl_leaf *leaf)
{
    int i;
    int level = leaf->node.level;

    for (i = level; i >= 0; i--)
        erase_one_level(list, &leaf->node, i);
}

/*
 * helper for skiplist_delete, this pushes pointers
 * around to remove a single slot
 */
static void delete_slot(struct sl_leaf *leaf, int slot)
{
    if (slot != leaf->nr - 1) {
        int i;
        for (i = slot; i <= leaf->nr - 1; i++) {
            leaf->keys[i] = leaf->keys[i + 1];
            leaf->ptrs[i] = leaf->ptrs[i + 1];
            smp_wmb();
        }
    } else if (leaf->nr > 1) {
        leaf->max = leaf->keys[leaf->nr - 2] + leaf->ptrs[leaf->nr - 2]->size;
        smp_wmb();
    }
    leaf->nr--;
}

/*
 * find a given [key, size] in the skiplist and remove it.
 * If we find anything, we return the slot pointer that
 * was stored in the tree.
 *
 * deletion involves a mostly lockless lookup to
 * find the right leaf.  Then we take the lock and find the
 * correct slot.
 *
 * The slot is removed from the leaf, and if the leaf
 * is now empty, it is removed from the skiplist.
 *
 * FIXME -- merge mostly empty leaves.
 */
struct sl_slot *skiplist_delete(struct sl_list *list, unsigned long key,
                                unsigned long size)
{
    struct sl_slot *slot_ret = NULL;
    struct sl_leaf *leaf;
    int slot;
    int ret;

    rcu_read_lock();
again:
    leaf = skiplist_lookup_leaf(list, NULL, key, size);
    if (!leaf)
        goto out;

    sl_lock_node(&leaf->node);
    if (!verify_key_in_leaf(leaf, key, size)) {
        sl_unlock_node(&leaf->node);
        goto again;
    }

    ret = leaf_slot_locked(leaf, key, size, &slot);
    if (ret == 0) {
        slot_ret = leaf->ptrs[slot];
    } else {
        sl_unlock_node(&leaf->node);
        goto out;
    }

    delete_slot(leaf, slot);
    if (leaf->nr == 0) {
        /*
		 * sl_erase has to mess wit the prev pointers, so
		 * we need to unlock it here
		 */
        leaf->node.dead = 1;
        sl_unlock_node(&leaf->node);
        sl_erase(list, leaf);
        sl_free_leaf(leaf);
    } else {
        sl_unlock_node(&leaf->node);
    }
out:
    rcu_read_unlock();
    return slot_ret;
}
EXPORT_SYMBOL(skiplist_delete);

int sl_init_list(struct sl_list *list, gfp_t mask)
{
    int i;

    list->head = kmalloc(sl_node_size(SKIP_MAXLEVEL), mask);
    if (!list->head)
        return -ENOMEM;
    sl_init_node(list->head, SKIP_MAXLEVEL);
    list->level = 0;
    spin_lock_init(&list->lock);

    for (i = 0; i < SKIP_MAXLEVEL; i++) {
        list->head->ptrs[i].next = NULL;
        list->head->ptrs[i].prev = NULL;
    }
    return 0;
}
EXPORT_SYMBOL(sl_init_list);

static int skiplist_callback(struct notifier_block *nfb, unsigned long action,
                             void *hcpu)
{
    int cpu = (long)hcpu;
    struct skip_preload *skp;
    struct sl_leaf *l;
    int level;
    int i;

    /* Free per-cpu pool of preloaded nodes */
    if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {
        skp = &per_cpu(skip_preloads, cpu);
        for (i = 0; i < SKIP_MAXLEVEL + 1; i++) {
            l = skp->preload[i];
            if (!l)
                continue;
            level = l->node.level;
            kmem_cache_free(slab_caches[level], l);
            skp->preload[i] = NULL;
        }
    }
    return NOTIFY_OK;
}

void __init skiplist_init(void)
{
    char buffer[16];
    int i;

    // hotcpu_notifier(skiplist_callback, 0);
    for (i = 0; i < SKIP_MAXLEVEL; i++) {
        snprintf(buffer, 16, "skiplist-%d", i);
        slab_caches[i] = kmem_cache_create(
            buffer, sl_leaf_size(i), 0,
            SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_TYPESAFE_BY_RCU, NULL);
    }
}
