#include <stdio.h>
#include <pthread.h>

#include "scoped_lock.h"

#define NR_THREAD 10

static int cnt = 0;

void *work(void *unused)
{
    {
        scoped_lock(SL_POSIX_MUTEX);
        cnt++;
    }

    pthread_exit(NULL);
}

int main(void)
{
    pthread_t p[NR_THREAD];
    int i;

    scoped_lock_init();

    for (i = 0; i < NR_THREAD; i++)
        pthread_create(&p[i], NULL, work, NULL);

    for (i = 0; i < NR_THREAD; i++)
        pthread_join(p[i], NULL);

    printf("cnt=%d\n", cnt);

    return 0;
}
