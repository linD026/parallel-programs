/* sequence lock
 * The write side must be non-preemptive or non-interrupted, it may lead
 * read side to become starvation.
 * 
 * See 
 * - https://en.wikipedia.org/wiki/Seqlock
 * - https://www.kernel.org/doc/html/latest/locking/seqlock.html
 */

#ifndef __SEQLOCK_H__
#define __SEQLOCK_H__

#include <stdatomic.h>

typedef struct seqlock {
    atomic_int seqcount;
    atomic_flag __write_lock;
} seqlock_t;

#define DEFINE_SEQLOCK(name)                         \
    seqlock_t name = { .seqnum = ATOMIC_VAR_INIT(0), \
                       .__write_lock = ATOMIC_FLAG_INIT }

static inline void write_seqlock(seqlock_t *lock)
{
    while (atomic_flag_test_and_set_explicit(&lock->__write_lock,
                                             memory_order_release))
        ;
    atomic_fetch_add_explicit(&lock->seqcount, 1, memory_order_release);
}

static inline void write_sequnlock(seqlock_t *lock)
{
    atomic_fetch_add_explicit(&lock->seqcount, 1, memory_order_release);
    atomic_flag_clear(&lock->__write_lock, memory_order_release);
}

/* Following are the reader operation
 * The default operations (seq_begin , seqretry, etc) are lockless.
 * To use the lock (blocking the other users) use the "_excl" postfix operations
 */

/* return the seq number for this time read
 * The seqcount must be even at beginning of the read side critical section.
 */
static inline void read_seqbegin(seqlock_t *lock)
{
    int seq;
    do {
        seq = atomic_load_explicit(&lock->seqcount, memory_order_consume);
    } while (!(seq & 0x1));
    return seq;
}

static inline bool read_seqretry(seqlock_t *lock, int seq)
{
    if (seq == atomic_load_explicit(&lock->seqcount, memory_order_acquire))
        return false;
    return true;
}

// reuse the "__write_lock" to make the mutual exclusion
static inline void read_seqlock_excl(seqlock_t *lock)
{
    while (atomic_flag_test_and_set_explicit(&lock->__write_lock,
                                             memory_order_release))
        ;
}

static inline void read_sequnlock_excl(seqlock_t *lock)
{
    atomic_flag_clear(&lock->__write_lock, memory_order_release);
}

/* The optimistic read_seqbegin, when the seqcount is odd number few times,
 * it turn into locked reader.
 * When the read side use these operation, the -1 value will store into seq.
 */
static inline void read_seqbegin_or_lock(seqlock_t *locki, int *seq)
{
    int seqcnt, try_cnt = 0;

     do {
        if (try_cnt > 10)
            goto locked;
        try_cnt++;
        seqcnt = atomic_load_explicit(&lock->seqcount, memory_order_consume);
    } while (!(seqcnt & 0x1));

    *seq = seqcnt;
    return;
locked:
    *seq = -1;
    read_seqlock_excl(lock);
}

static inline bool need_seqretry(seqlock_t *lock, int seq)
{
    // read side is locking so the data is newest.
    if (seq == -1)
        return false;
   return read_seqretry(lock, seq);
}

static inline void done_seqretry(seqlock_t *lock, int seq)
{
    if (seq != -1)
        return;
    read_seqlock_excl(lock);
}

#endif /* __SEQLOCK_H__ */
