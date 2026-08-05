#ifndef CO_CONTEXT_H
#define CO_CONTEXT_H

#if defined(_WIN32)

#   include "platform/Windows_x64/co_context_win64.h"

#elif defined(__x86_64__) || defined(__amd64__)

#   include "platform/Linux_x86/co_context_sys.h"
#elif defined(__linux__) && defined(__aarch64__)

#   include "platform/linux_aarch64/co_context_aarch64.h"

/* 其他平台之後支援 */
#else

#   error "Unsupported context backend"

#endif

#endif