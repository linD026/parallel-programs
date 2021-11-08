/* 
 * The common time tracer of tools
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

#ifndef __TRACE_TIME_H__
#define __TRACE_TIME_H__

#if defined(CONFIG_TRACE_TIME)
#include <time.h>

#define time_diff(start, end)                                                  \
    (end.tv_nsec - start.tv_nsec < 0 ?                                         \
             (1000000000 + end.tv_nsec - start.tv_nsec) :                      \
             (end.tv_nsec - start.tv_nsec))
#define time_check(_FUNC_)                                                     \
    do {                                                                       \
        struct timespec time_start;                                            \
        struct timespec time_end;                                              \
        double during;                                                         \
        clock_gettime(CLOCK_MONOTONIC, &time_start);                           \
        _FUNC_;                                                                \
        clock_gettime(CLOCK_MONOTONIC, &time_end);                             \
        during = time_diff(time_start, time_end);                              \
        printf("[trace time] %s: %f ns\n", #_FUNC_, during);                   \
    } while (0)
#define __time_check(_FUNC_)                                                   \
    do {                                                                       \
        struct timespec time_start;                                            \
        struct timespec time_end;                                              \
        double during;                                                         \
        clock_gettime(CLOCK_MONOTONIC, &time_start);                           \
        _FUNC_;                                                                \
        clock_gettime(CLOCK_MONOTONIC, &time_end);                             \
        during = time_diff(time_start, time_end);                              \
        printf("%f\n", #_FUNC_, during);                                       \
    } while (0)
#define time_check_return(_FUNC_)                                              \
    ({                                                                         \
        struct timespec time_start;                                            \
        struct timespec time_end;                                              \
        double during;                                                         \
        clock_gettime(CLOCK_MONOTONIC, &time_start);                           \
        _FUNC_;                                                                \
        clock_gettime(CLOCK_MONOTONIC, &time_end);                             \
        during = time_diff(time_start, time_end);                              \
        during;                                                                \
    })
#define time_check_loop(_FUNC_, times)                                         \
    do {                                                                       \
        double sum = 0;                                                        \
        int i;                                                                 \
        for (i = 0; i < times; i++)                                            \
            sum += time_check_return(_FUNC_);                                  \
        printf("[trace time] loop %d : %f ns\n", times, sum);                  \
    } while (0)
#define time_check_loop_return(_FUNC_, times)                                  \
    ({                                                                         \
        double sum = 0;                                                        \
        int i;                                                                 \
        for (i = 0; i < times; i++)                                            \
            sum += time_check_return(_FUNC_);                                  \
        sum;                                                                   \
    })
#else /* CONFIG_TRACE_TIME */
#define time_diff(start, end)
#define time_check(_FUNC_)                                                     \
    do {                                                                       \
        _FUNC_;                                                                \
    } while (0)
#define __time_check(_FUNC_)                                                   \
    do {                                                                       \
        _FUNC_;                                                                \
    } while (0)
#define time_check_return(_FUNC_) ({ _FUNC_; })
#define time_check_loop(_FUNC_, times)                                         \
    do {                                                                       \
            _FUNC_;                                                            \
    } while (0)
#define time_check_loop_return(_FUNC_, times)                                  \
    ({                                                                         \
            _FUNC_;                                                            \
    })

#endif /* CONFIG_TRACE_TIME */
#endif /* __TRACE_TIME_H__ */
