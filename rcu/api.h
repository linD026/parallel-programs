/* 
 * The common multi-thread API for userspace RCU 
 *
 * Wrap the pthread lock API for building the Linux kernel style.
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

#ifndef __RCU_COMMON_API_H__
#define __RCU_COMMON_API_H__

/* lock primitives from pthread and compiler primitives */

#include <pthread.h>

typedef pthread_mutex_t spinlock_t;

#define DEFINE_SPINLOCK(lock) spinlock_t lock = PTHREAD_MUTEX_INITIALIZER
#define SPINLOCK_INIT PTHREAD_MUTEX_INITIALIZER

static __inline__ void spin_lock_init(spinlock_t *sp)
{
    int ret;

    ret = pthread_mutex_init(sp, NULL);
    if (ret != 0) {
        fprintf(stderr, "spin_lock_init:pthread_mutex_init %d\n", ret);
        abort();
    }
}

static __inline__ void spin_lock(spinlock_t *sp)
{
    int ret;

    ret = pthread_mutex_lock(sp);
    if (ret != 0) {
        fprintf(stderr, "spin_lock:pthread_mutex_lock %d\n", ret);
        abort();
    }
}

static __inline__ void spin_unlock(spinlock_t *sp)
{
    int ret;

    ret = pthread_mutex_unlock(sp);
    if (ret != 0) {
        fprintf(stderr, "spin_unlock:pthread_mutex_unlock %d\n", ret);
        abort();
    }
}

#define ACCESS_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#define READ_ONCE(x)                         \
    ({                                       \
        __typeof__(x) ___x = ACCESS_ONCE(x); \
        ___x;                                \
    })
#define WRITE_ONCE(x, val)      \
    do {                        \
        ACCESS_ONCE(x) = (val); \
    } while (0)
#define barrier() __asm__ __volatile__("" : : : "memory")
#define __allow_unused __attribute__((unused))
#define smp_mb() __asm__ __volatile__("mfence" : : : "memory")
#if defined(__linux__)
#define current_tid() (int)gettid()
#else
#define current_tid() (int)pthread_self()
#endif

#endif /* __RCU_COMMON_API_H__ */
