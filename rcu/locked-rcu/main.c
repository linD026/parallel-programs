#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "rcupdate.h"

struct test {
    int count;
};

RCU_DEFINE(rcu_head);

void *reader_side(void *argv)
{
    struct test *tmp;

    rcu_read_lock(rcu_head);

    tmp = rcu_dereference(rcu_head);

    printf("[reader %d] %d\n", gettid(), tmp->count);

    rcu_read_unlock(rcu_head);

    pthread_exit(NULL);
}

void *updater_side(void *argv)
{
    struct test *newval = (struct test *)malloc(sizeof(struct test));
    newval->count = (int)gettid();

    printf("[updater %d]\n", newval->count);

    rcu_assign_pointer(&rcu_head, (void *)newval);

    synchronize_rcu(&rcu_head);

    pthread_exit(NULL);
}

#define READER_NUM 10
#define UPDATER_NUM 1

int main(int argc, char *argv[])
{
    pthread_t reader[READER_NUM];
    pthread_t updater[UPDATER_NUM];
    int i;
	struct test *obj = (struct test *)malloc(sizeof(struct test));

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

    return 0;
}
