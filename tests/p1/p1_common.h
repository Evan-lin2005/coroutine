#ifndef P1_COMMON_H
#define P1_COMMON_H

#include "../p0/p0_common.h"

#define P1_RESUME(co)                    co_resume((co), NULL, NULL)
#define P1_RESUME_IN(co, input)          co_resume((co), (input), NULL)
#define P1_YIELD()                       co_yield_now(NULL, NULL)
#define P1_YIELD_OUT(output)             co_yield_now((output), NULL)
#define P1_YIELD_NEXT(next_input)        co_yield_now(NULL, (next_input))
#define P1_YIELD_OUT_NEXT(output, next)  co_yield_now((output), (next))

#endif /* P1_COMMON_H */
