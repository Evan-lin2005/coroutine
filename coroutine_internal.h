#ifndef COROUTINE_INTERNAL_H
#define COROUTINE_INTERNAL_H

#include "coroutine.h"
#include "co_context.h"
#include <stddef.h>
#include <stdint.h>


struct co_stack {
    void   *base;
    void   *lo, *hi;
    size_t  total;
    int     external; /* 1：base 由呼叫端擁有，destroy 不 munmap/VirtualFree */
};

enum co_state { CO_READY, CO_RUNNING, CO_SUSPENDED, CO_DONE , CO_WAITING};

struct coroutine {
    struct co_context context;
    struct co_stack   stack;
    co_function       function;
    void             *userdata;
    void             *mailbox;
    struct coroutine *caller;
    enum co_state     state;
    int               cancelling; /* co_cancel 進行中；違約 yield 時回 CANCEL_IGNORED */
    /* process 生命週期內唯一、不因執行緒結束而重用的 owner 序號（0=未綁定／已清） */
    uint64_t          owner_id;
    void *  storage_buffer;
    size_t  st_cap;
    void   *cls[CO_CLS_SLOTS];
    /* create 時快照；destroy 必須用同一套 alloc/free，避免中途換 g_allocator 造成 mismatch */
    co_allocator      allocator;
    /* per-thread 存活鏈（僅 heap 協程；thread exit 時 orphan reclaim 用） */
    struct coroutine *live_next;
#ifdef __SANITIZE_ADDRESS__
#  define CO_ASAN_FIELDS 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define CO_ASAN_FIELDS 1
#  endif
#endif
#ifdef CO_ASAN_FIELDS
    const void       *asan_stack_bottom; /* main：平台 API；fiber：可與 stack.lo 並存 */
    size_t            asan_stack_size;
    void             *asan_fake_stack;   /* start_switch 保存；finish_switch 還原 */
#endif
};

void co_context_switch(struct co_context *from, struct co_context *to);
void co_trampoline_entry(void);
void co_trampoline_body(void);
void co_bad_return(void);

int  co_platform_initialize(void);
/* opt-in：安裝 SIGSEGV/SIGBUS（或 Windows VEH）診斷；ASan 下為 no-op */
int  co_platform_install_crash_handler(void);
/* 查詢目前 OS 執行緒主堆疊邊界（ASan main fiber 用）；成功回 0 */
int  co_platform_query_thread_stack(const void **bottom, size_t *size);
int  co_stack_create(struct co_stack *s, size_t want);
int  co_stack_create_from(struct co_stack *s, void *base, size_t total);
void co_stack_destroy(struct co_stack *s);
void initialize_context(struct coroutine *co);

#endif /* COROUTINE_INTERNAL_H */