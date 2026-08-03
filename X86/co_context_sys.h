#ifndef CO_CONTEXT_SYS_H
#define CO_CONTEXT_SYS_H

#include <stdint.h>

struct co_context {
    void     *rsp;
    uint64_t  rbx, rbp, r12, r13, r14, r15;
    uint32_t  mxcsr;
    uint16_t  x87cw;
    uint16_t  pad;
};

#endif