#ifndef CO_CONTEXT_WINDOWS_H
#define CO_CONTEXT_WINDOWS_H


#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>


struct co_context {
    
    void     *rsp;                 /* +0 */
    uint64_t  rbx, rbp, rdi, rsi;  /* +8 .. +32 */
    uint64_t  r12, r13, r14, r15;  /* +40 .. +64 */
    uint32_t  mxcsr;               /* +72 */
    uint16_t  x87cw;               /* +76 */
    uint16_t  pad;                 /* +78 */
    /* 16-byte 對齊後再放 XMM*/
    _Alignas(16) uint8_t   xmm[10][16];         /* xmm6..xmm15，起始偏移對齊到 16 */
};

#endif