#ifndef __SKIPLIST_H__
#define __SKIPLIST_H__

/* total number of node is 2^32
 * the level here is log2(n), which is log2(2^32) = 32
 */
#define SL_MAXLEVEL 32

struct sl_link {
    struct sl_link *prev;
    struct sl_link *next;
};

struct sl_list {
    int size;
    int level;
    struct sl_link head[SL_MAXLEVEL];
};

struct sl_list *sl_list_alloc(void);
void sl_delete(struct sl_list *list);
void *sl_search(struct sl_list *list, int key);
int sl_insert(struct sl_list *list, int key, void *val);
int sl_erase(struct sl_list *list, int key);

#endif /* __SKIPLIST_H__ */
