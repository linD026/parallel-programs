/* 
 * thrd-RCU list: A benchmark of thrd-rcu linked list
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

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "rculist.h"

struct test {
    int count;
    struct list_head node;
};

static struct list_head head;

static struct test *test_alloc(int val)
{
    struct test *new = (struct test *)malloc(sizeof(struct test));
    if (!new) {
        fprintf(stderr, "test_alloc failed\n");
        abort();
    }

    new->count = val;
    list_init_rcu(&new->node);

    return new;
}

static void *reader_side(void *argv)
{
    struct test __allow_unused *tmp;
    struct list_head *node;

    rcu_init();

    rcu_read_lock();

    list_for_each(node, &head)
    {
        tmp = list_entry_rcu(node, struct test, node);
    }

    rcu_read_unlock();

    pthread_exit(NULL);
}

static void *updater_side(void *argv)
{
    struct test *newval = test_alloc(current_tid());

    list_add_tail_rcu(&newval->node, &head);
    synchronize_rcu();

    pthread_exit(NULL);
}

#define READER_NUM 10
#define UPDATER_NUM 1

static __inline__ void benchmark(void)
{
    pthread_t reader[READER_NUM];
    pthread_t updater[UPDATER_NUM];
    struct list_head *node, *pos;
    struct test *tmp;
    int i;
    list_init_rcu(&head);

    for (i = 0; i < READER_NUM / 2; i++)
        pthread_create(&reader[i], NULL, reader_side, NULL);

    for (i = 0; i < UPDATER_NUM; i++)
        pthread_create(&updater[i], NULL, updater_side, NULL);

    for (i = READER_NUM / 2; i < READER_NUM; i++)
        pthread_create(&reader[i], NULL, reader_side, NULL);

    for (i = 0; i < READER_NUM; i++)
        pthread_join(reader[i], NULL);

    for (i = 0; i < UPDATER_NUM; i++)
        pthread_join(updater[i], NULL);

    list_for_each_safe(pos, node, &head)
    {
        tmp = container_of(pos, struct test, node);
        free(tmp);
    }
    list_init_rcu(&head);

    rcu_clean();
}

#include "../trace_timer.h"

int main(int argc, char *argv[])
{
    time_check_loop(benchmark(), 1000);
    return 0;
}
