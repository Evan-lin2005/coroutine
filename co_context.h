#ifndef CO_CONTEXT_H
#define CO_CONTEXT_H

#if defined(_WIN32)

#   include "X86/co_context_win64.h"

#elif defined(__x86_64__) || defined(__amd64__)

#   include "X86/co_context_sys.h"

/* 其他平台之後支援 */
#else

#   error "Unsupported context backend"

#endif

#endif