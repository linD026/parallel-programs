#include <stdio.h>
#include <pthread.h>

#include "tsm.h"

#define NR_THREAD 32

DEFINE_TSM(tsm);
pthread_t data;

void *work(void *unused)
{
	tsm_local_key_t k;
retry:
	k = begin_tsm(&tsm);
	
	/* Assume this will write into the register.
	 * It will commit to the desired data.
	 */
	WRITE_ONCE(&data, pthread_self());

	/* In hardware design, "commit_tsm()" means it will check
	 * whether the data is already wrote back to the storage.
	 * If yes than this commit will false.
	 */	
	if (!commit_tsm(&tsm, k))
		goto retry;

	printf("[%lu] write success\n", (unsigned long) pthread_self());

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

	printf("result is %lu\n", (unsigned long) data);

    return 0;
}
