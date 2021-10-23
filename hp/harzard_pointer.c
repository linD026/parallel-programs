#include <assert.h>
#include <stdlib.h>
#include <stdalign.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <threads.h>

#define HP_MAX_THREAD_RL 128
#define HP_MAX_PTR 4
#define COHERENCE_PAD 128
#define HP_TID_UNINIT -1

typedef struct {
    size_t size;
    uintptr_t *list;
} retirelist_t;

typedef struct {
    alignas(COHERENCE_PAD / 2) atomic_uintptr_t hp[HP_MAX_PTR];
    alignas(COHERENCE_PAD / 2) retirelist_t rl;
} th_info_t;

typedef struct hp_struct {
    th_info_t thread_info[HP_MAX_THREAD_RL];
    void (*delete_func)(void *);
} hp_t;

static thread_local int tid = HP_TID_UNINIT;
static atomic_int_fast32_t __tid = ATOMIC_VAR_INIT(HP_TID_UNINIT);

#define __update_tid()                                                    \
    ({                                                                    \
        tid = atomic_fetch_add_explicit(&__tid, 1, memory_order_consume); \
        assert(tid < HP_MAX_THREAD_RL);                                   \
        tid;                                                              \
    })

static inline int get_tid(void)
{
    return (tid == HP_TID_UNINIT) ? __update_tid() : tid;
}

hp_t *hp_new(void (*delete_func)(void *))
{
    int i, j;
    hp_t *hp = aligned_alloc(COHERENCE_PAD, sizeof(hp_t));

    assert(hp);
    hp->delete_func = delete_func;
    for (i = 0; i < HP_MAX_THREAD_RL; i++) {
        hp->thread_info[i].rl.list =
            calloc(HP_MAX_THREAD_RL, sizeof(uintptr_t));
        assert(hp->thread_info[i].rl.list);
        for (j = 0; j < HP_MAX_PTR; j++)
            atomic_init(&hp->thread_info[i].hp[j], 0);
    }

    return hp;
}

void hp_destory(hp_t *hp)
{
    int i;
    assert(hp);

    for (i = 0; i < HP_MAX_THREAD_RL; i++)
        free(hp->thread_info[i].rl.list);
    free(hp);
}

void hp_retirelist(hp_t *hp, uintptr_t ptr)
{
    int i, j, k;
    bool freeable;
    uintptr_t obj;
    th_info_t *thi = &hp->thread_info[get_tid()];
    thi->rl.list[thi->rl.size++] = ptr;
    assert(thi->rl.size < HP_MAX_THREAD_RL);

    for (i = 0; i < HP_MAX_THREAD_RL; i++) {
        freeable = true;
        obj = thi->rl.list[i];
        for (j = 0; j < HP_MAX_THREAD_RL && freeable; j++) {
            for (k = 0; k < HP_MAX_PTR; k++) {
                if (atomic_load(&hp->thread_info[j].hp[k]) == obj) {
                    freeable = false;
                    break;
                }
            } /* for each hp in thread_info[j] */
        } /* for each thread_info in hp_t */
        if (freeable) {
            memmove(&thi->rl.list[i], &thi->rl.list[--thi->rl.size],
                    sizeof(uintptr_t));
            hp->delete_func((void *)obj);
        }
    } /* for each uintptr in thi->rl */
}

static inline uintptr_t __hp_protect_release(hp_t *hp, int hp_index,
                                             uintptr_t ptr)
{
    atomic_store_explicit(&hp->thread_info[get_tid()].hp[hp_index], ptr,
                          memory_order_release);
    return ptr;
}

uintptr_t hp_protect_release(hp_t *hp, int hp_index, uintptr_t ptr)
    __attribute__((alias("__hp_protect_release")));

static inline void __hp_protect_clear(hp_t *hp)
{
    int i, j;
    for (i = 0; i < HP_MAX_THREAD_RL; i++)
        for (j = 0; j < HP_MAX_PTR; j++)
            atomic_store(&hp->thread_info[i].hp[j], 0);
    atomic_thread_fence(memory_order_release);
}

void hp_protect_clear(hp_t *hp) __attribute__((alias("__hp_protect_clear")));
