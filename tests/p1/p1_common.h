#ifndef P1_COMMON_H
#define P1_COMMON_H

#include "../p0/p0_common.h"

#define P1_RESUME(co)                    co_resume((co), NULL, NULL)
#define P1_RESUME_IN(co, input)          co_resume((co), (input), NULL)
#define P1_YIELD()                       co_yield_now(NULL, NULL)
#define P1_YIELD_OUT(output)             co_yield_now((output), NULL)
#define P1_YIELD_NEXT(next_input)        co_yield_now(NULL, (next_input))
#define P1_YIELD_OUT_NEXT(output, next)  co_yield_now((output), (next))

/* 相容舊巨集名 */
#define P1_RESUME_ARG(co, arg)           P1_RESUME_IN((co), (arg))
#define P1_YIELD_ARG(arg)                P1_YIELD_OUT((arg))
#define P1_YIELD_OUT_PTR(out)            P1_YIELD_NEXT((out))
#define P1_YIELD_ARG_OUT(arg, out)       P1_YIELD_OUT_NEXT((arg), (out))

#endif /* P1_COMMON_H */
