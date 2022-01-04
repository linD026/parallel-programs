#include <stdio.h>
#include <pthread.h>

#include "centralized_barrier.h"

#define NR_THREAD 32

DEFINE_BARRIER(b);

void *work(void *unused)
{
    printf("[1]\n");
    barrier(&b, NR_THREAD);
    printf("[2]\n");

    pthread_exit(NULL);
}

int main(void)
{
    pthread_t p[NR_THREAD];
    int i;

    for (i = 0; i < NR_THREAD; i++)
        pthread_create(&p[i], NULL, work, NULL);

    for (i = 0; i < NR_THREAD; i++)
        pthread_join(p[i], NULL);

    return 0;
}
