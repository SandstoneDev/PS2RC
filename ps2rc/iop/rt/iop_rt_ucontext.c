/* iop_rt_ucontext.c - IopCoroOps over POSIX ucontext (host tools and tests). Stacks come from malloc. */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <ucontext.h>
#include "iop_rt.h"
#include "iop_rt_ucontext.h"

static ucontext_t main_uc;

static void *uc_create(void *user, size_t stack, void (*entry)(void))
{
    (void)user;
    ucontext_t *uc = (ucontext_t *)calloc(1, sizeof *uc);
    void *st = malloc(stack);
    if (!uc || !st) { free(uc); free(st); return 0; }
    getcontext(uc);
    uc->uc_stack.ss_sp = st;
    uc->uc_stack.ss_size = stack;
    uc->uc_link = 0;
    makecontext(uc, entry, 0);
    return uc;
}

static void uc_swap(void *user, void *from, void *to)
{
    (void)user;
    swapcontext((ucontext_t *)from, (ucontext_t *)to);
}

static void *uc_main(void *user) { (void)user; return &main_uc; }

void iop_rt_coro_ucontext(IopCoroOps *ops)
{
    ops->user = 0;
    ops->create = uc_create;
    ops->swap = uc_swap;
    ops->main_ctx = uc_main;
}
