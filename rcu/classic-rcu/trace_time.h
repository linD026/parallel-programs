#ifndef __TRACE_TIME_H__
#define __TRACE_TIME_H__

#include <linux/ktime.h>

#define TRACE_LIMIT 20

enum { TRACE_START, TRACE_END, TRACE_FINISH, TRACE_NOTHING };

struct trace_time {
    const char *name;
    ktime_t start;
    ktime_t end;
    s64 during;
    int flag;
    int number;
};

static atomic_t __trace_time_cnt = ATOMIC_INIT(0);

static void __trace_time_print(struct trace_time *trace)
{
    pr_info("trace %s number %d: [CPU#%d], %llu usec\n", trace->name,
            trace->number, smp_processor_id(),
            (unsigned long long)trace->during >> 10);
}

#define TRACE_TIME_INIT(n)                                  \
    ({                                                      \
        struct trace_time __tt = {                          \
            .name = n,                                      \
            .during = 0,                                    \
            .number = atomic_inc_return(&__trace_time_cnt), \
            .flag = TRACE_NOTHING,                          \
        };                                                  \
        __tt;                                               \
    })

#define TRACE_TIME_START(t)                                          \
    do {                                                             \
        if ((t).number < TRACE_LIMIT && (t).flag == TRACE_NOTHING) { \
            (t).start = ktime_get();                                 \
            (t).flag = TRACE_START;                                  \
        }                                                            \
    } while (0)

#define TRACE_TIME_END(t)                                          \
    do {                                                           \
        if ((t).number < TRACE_LIMIT && (t).flag == TRACE_START) { \
            (t).end = ktime_get();                                 \
            (t).flag = TRACE_END;                                  \
        }                                                          \
    } while (0)

#define TRACE_CALC(t)                                                \
    do {                                                             \
        if ((t).number < TRACE_LIMIT && (t).flag == TRACE_END) {     \
            (t).during = ktime_to_ns(ktime_sub((t).end, (t).start)); \
            (t).flag = TRACE_FINISH;                                 \
        }                                                            \
    } while (0)

#define TRACE_PRINT(t)                                              \
    do {                                                            \
        if ((t).number < TRACE_LIMIT && (t).flag == TRACE_FINISH) { \
            __trace_time_print(&(t));                               \
            (t).flag = TRACE_NOTHING;                               \
        }                                                           \
    } while (0)

#endif /*__TRACE_TIME_H__ */
