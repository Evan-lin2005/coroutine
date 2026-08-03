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
};

enum co_state { CO_READY, CO_RUNNING, CO_SUSPENDED, CO_DONE , CO_WAITING};

struct coroutine {
    struct co_context context;
    struct co_stack   stack;
    co_function       function;
    void             *argument;
    struct coroutine *caller;
    enum co_state     state;
    const void       *owner_token;
};

void co_context_switch(struct co_context *from, struct co_context *to);
void co_trampoline_entry(void);
void co_trampoline_body(void);
void co_bad_return(void);

int  co_platform_initialize(void);
int  co_stack_create(struct co_stack *s, size_t want);
void co_stack_destroy(struct co_stack *s);
void initialize_context(struct coroutine *co);

#endif /* COROUTINE_INTERNAL_H */