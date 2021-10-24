/* 
 * skiplist: The benchmark of sequence program of skip list implementation
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

#include <stdio.h>
#include <assert.h>

#include "skiplist.h"

#define test(ops, index, size) \
    printf("[" #ops "]: "      \
           "return %d, "       \
           "index %d, "        \
           "size %d\n",        \
           (int)ops, index, size)

#define times 10000

int main(int argc, char *argv[])
{
    struct sl_list *list = sl_list_alloc();
    int i, arr[10] = { 0 };
    for (i = 0; i < times; i++)
        assert(sl_insert(list, i, &arr[i]) == 0);
    //test(sl_insert(list, i, NULL), i, list->size);

    for (i = 0; i < 10; i++) {
        arr[i] = i;
        printf("%d search %d\n", i, *(int *)sl_search(list, i));
    }

    for (i = 0; i < times; i++)
        assert(sl_erase(list, i) == 0);
    //test(sl_erase(list, i), i, list->size);

    sl_delete(list);
    return 0;
}
