/* 
 * Read side benchmark: A benchmark of per-thread of refcnt RCU
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

#include "thrd_rcu.h"
#include "../trace_timer.h"

struct test {
    int count;
};

static struct test __rcu *foo;

static __inline__ void read_rcu(void)
{
    struct test __allow_unused *tmp;

    rcu_read_lock();

    tmp = rcu_dereference(foo);

    rcu_read_unlock();
}

static void *reader_side(void *argv)
{
    rcu_init();

    time_check_loop(read_rcu(), TRACE_LOOP);

    smp_mb();

    pthread_exit(NULL);
}

static void *updater_side(void *argv)
{
    struct test *oldp;
    struct test *newval = (struct test *)malloc(sizeof(struct test));
    newval->count = current_tid();

    //printf("[updater %d]\n", newval->count);

    oldp = rcu_assign_pointer(foo, newval);

    synchronize_rcu();
    free(oldp);

    pthread_exit(NULL);
}

static __inline__ void benchmark(void)
{
    pthread_t reader[READER_NUM];
    pthread_t updater[UPDATER_NUM];
    int i;
    foo = (struct test __rcu *)malloc(sizeof(struct test));
    rcu_uncheck(foo)->count = 0;

    smp_mb();

    for (i = 0; i < READER_NUM / 2; i++)
        pthread_create(&reader[i], NULL, reader_side, NULL);

    for (i = 0; i < UPDATER_NUM; i++)
        pthread_create(&updater[i], NULL, updater_side, NULL);

    for (i = READER_NUM / 2; i < READER_NUM; i++)
        pthread_create(&reader[i], NULL, reader_side, NULL);

    for (i = 0; i < UPDATER_NUM; i++)
        pthread_join(updater[i], NULL);

    for (i = 0; i < READER_NUM; i++)
        pthread_join(reader[i], NULL);

    free(rcu_uncheck(foo));

    rcu_clean();
}

int main(int argc, char *argv[])
{
    printf("thrd rcu read side: reader %d, updater %d\n", READER_NUM,
           UPDATER_NUM);
    benchmark();
    return 0;
}
