/*
 * harzard pointer: An array-based simple harzard pointer
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
#ifndef __HAZARD_POINTER_H__
#define __HAZARD_POINTER_H__

#include <stdint.h>

typedef struct hp_struct hp_t;

hp_t *hp_new(void (*delete_func)(void *));
void hp_destory(hp_t *hp);
void hp_retirelist(hp_t *hp, uintptr_t ptr);
uintptr_t hp_protect_release(hp_t *hp, int hp_index, uintptr_t ptr);
void hp_protect_clear(hp_t *hp);

#define HP_DEFINE4(n0, n1, n2, n3) \
    enum {                         \
        HP_##n0 = 0,               \
        HP_##n1 = 1,               \
        HP_##n2 = 2,               \
        HP_##n3 = 3,               \
    };

#endif /* __HAZARD_POINTER_H__ */
