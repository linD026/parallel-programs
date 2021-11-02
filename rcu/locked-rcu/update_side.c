/* 
 * Update side benchmark: A benchmark of global reference count of simple RCU
 *
 * Use the memory model from C11 standard and wraping the pthread lock API to
 * to build the Linux kernel API.
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

#include "rcupdate.h"
#include "../trace_timer.h"

#if defined(__linux__)
#define current_tid() (int)gettid()
#else
#define current_tid() 0xFFFF & (unsigned long)pthread_self()
#endif

struct test {
    int count;
};

RCU_DEFINE(rcu_head);

void *reader_side(void *argv)
{
    struct test __allow_unused *tmp;

    rcu_read_lock(rcu_head);

    tmp = rcu_dereference(rcu_head);

    rcu_read_unlock(rcu_head);

    pthread_exit(NULL);
}

static __inline__ void update_rcu(void)
{
    struct test *newval = (struct test *)malloc(sizeof(struct test));
    newval->count = current_tid();

    rcu_assign_pointer(&rcu_head, (void *)newval);

    synchronize_rcu(&rcu_head);
}

void *updater_side(void *argv)
{
    time_check_loop(update_rcu(), 1000);

    pthread_exit(NULL);
}

#define READER_NUM 100
#define UPDATER_NUM 5

static __inline__ void benchmark(void)
{
    pthread_t reader[READER_NUM];
    pthread_t updater[UPDATER_NUM];
    int i;
    struct test *obj = (struct test *)malloc(sizeof(struct test));
    obj->count = 0;

    rcu_init(obj, &rcu_head);

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

    rcu_free(&rcu_head);
}

#include "../trace_timer.h"

int main(int argc, char *argv[])
{
    printf("locked rcu update side: reader %d, updater %d\n", READER_NUM, UPDATER_NUM);
    benchmark();
    return 0;
}
