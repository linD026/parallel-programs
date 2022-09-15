#ifndef __SCOPED_LOCK_H__
#define __SCOPED_LOCK_H__

#include <assert.h>
#include <stdbool.h>

typedef struct scoped_lock_struct {
    unsigned int type_flags;
    unsigned int id;
} scoped_lock_t;

#define SCOPED_LOCK_MAX_SIZE 32

/* POSIX thread library */
#include <pthread.h>

#define SL_POSIX_MUTEX 0x0001

struct sl_posix_mutex {
    pthread_mutex_t lock;
    unsigned int id;
};

int sl_acquire_posix_mutex_lock(struct sl_posix_mutex *sl_pmutexs,
                                scoped_lock_t *lock)
{
    int i = lock->id & (SCOPED_LOCK_MAX_SIZE - 1);
    unsigned int temp;
    //for (i = 0; i < SCOPED_LOCK_MAX_SIZE; i++) {
    temp = __atomic_load_n(&sl_pmutexs[i].id, __ATOMIC_RELAXED);

    if (0 == temp || lock->id == temp) {
        pthread_mutex_lock(&sl_pmutexs[i].lock);
        __atomic_store_n(&sl_pmutexs[i].id, lock->id, __ATOMIC_RELEASE);
        return 0;
    }
    //}
    return -1;
}

int sl_release_posix_mutex_lock(struct sl_posix_mutex *sl_pmutexs,
                                scoped_lock_t *lock)
{
    int i = lock->id & (SCOPED_LOCK_MAX_SIZE - 1);
    //for (i = 0; i < SCOPED_LOCK_MAX_SIZE; i++) {
    if (lock->id == __atomic_load_n(&sl_pmutexs[i].id, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&sl_pmutexs[i].id, 0, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&sl_pmutexs[i].lock);
        return 0;
    }
    //}
    return -1;
}

/* scoped lock type mask */
#define SL_TYPE_LOCK_MASK (SL_POSIX_MUTEX)

struct scoped_lock_data {
    struct sl_posix_mutex sl_posix_mutex[SCOPED_LOCK_MAX_SIZE];
};
static struct scoped_lock_data scoped_lock_data;

void scoped_lock_lock(scoped_lock_t *lock)
{
    int ret;

    if (lock->type_flags & SL_POSIX_MUTEX) {
        ret = sl_acquire_posix_mutex_lock(&scoped_lock_data.sl_posix_mutex[0],
                                          lock);
        if (ret)
            assert(false);
        return;
    }

    assert(false);
}

void scoped_lock_unlock(scoped_lock_t *lock)
{
    int ret;

    if (lock->type_flags & SL_POSIX_MUTEX) {
        ret = sl_release_posix_mutex_lock(&scoped_lock_data.sl_posix_mutex[0],
                                          lock);
        if (ret)
            assert(false);
        return;
    }

    assert(false);
}

#define scoped_lock(type)                                               \
    scoped_lock_t __s_l_v __attribute__((cleanup(scoped_lock_unlock))); \
    __s_l_v.id = __COUNTER__ + 1;                                       \
    __s_l_v.type_flags = type;                                          \
    do {                                                                \
        assert((type & SL_TYPE_LOCK_MASK));                             \
        scoped_lock_lock(&__s_l_v);                                     \
    } while (0)

void scoped_lock_init(void)
{
    int i;

    for (i = 0; i < SCOPED_LOCK_MAX_SIZE; i++) {
        pthread_mutex_init(&scoped_lock_data.sl_posix_mutex[i].lock, NULL);
    }
}

#endif /* __SCOPED_LOCK_H__ */
