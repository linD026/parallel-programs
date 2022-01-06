/* tsm: software transactional memory
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
 * Copyright (C) 2022 linD026
 */

#ifndef __TSM_H__
#define __TSM_H__

#include <stdatomic.h>
#include <stdbool.h>

struct tsm {
    atomic_ulong tick;
};

typedef unsigned long tsm_local_key_t;

#define TSM_INIT                   \
    {                              \
        .tick = ATOMIC_VAR_INIT(0) \
    }

#define DEFINE_TSM(n) struct tsm n = TSM_INIT

#ifndef WRITE_ONCE
#define WRITE_ONCE(p, v)                                         \
    do {                                                         \
        atomic_store_explicit((_Atomic(typeof(*(p))) *)(p), (v), \
                              memory_order_relaxed);             \
    } while (0)
#endif /* WRITE_ONCE */

static inline tsm_local_key_t begin_tsm(struct tsm *tp)
{
    return atomic_fetch_add_explicit(&tp->tick, 1, memory_order_seq_cst);
}

static inline bool commit_tsm(struct tsm *tp, tsm_local_key_t lk)
{
    tsm_local_key_t ttk = atomic_load(&tp->tick);
    if (ttk == lk + 1)
        return true;
    return false;
}

#endif /* __TSM_H__ */
