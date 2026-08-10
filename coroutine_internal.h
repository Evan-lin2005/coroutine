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
    const void       *owner_token;
    void *  storage_buffer;
    size_t  st_cap;
    void   *cls[CO_CLS_SLOTS];
    /* create 時快照；destroy 必須用同一套 alloc/free，避免中途換 g_allocator 造成 mismatch */
    co_allocator      allocator;
};

void co_context_switch(struct co_context *from, struct co_context *to);
void co_trampoline_entry(void);
void co_trampoline_body(void);
void co_bad_return(void);

int  co_platform_initialize(void);
int  co_stack_create(struct co_stack *s, size_t want);
int  co_stack_create_from(struct co_stack *s, void *base, size_t total);
void co_stack_destroy(struct co_stack *s);
void initialize_context(struct coroutine *co);

#endif /* COROUTINE_INTERNAL_H */