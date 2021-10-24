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
