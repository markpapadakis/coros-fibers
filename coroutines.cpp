#include "coroutines.h"
#include <ansifmt.h>
#include <switch.h>
#include <switch_print.h>
#include <text.h>

thread_local coro_ctx Fibers::mainCoro;
thread_local Fibers::Scheduler Fibers::scheduler;

#pragma mark Coroutines
// A coroutine trampoline function _never_ returns
// When the function that implements the coroutine returns, we are in the context of the coroutine (stack)
// So, to return control to the callee, we 'd need to transfer control to the calle's context(i.e restore regs etc)
//
// This makes little to no sense though.
// Instead, because we know the coro has ended, we should resume a coro (if one) waiting for it, or just resume the next runnable coro.
// There should be always be one runnable coro, at least the one associated with the main run-loop of the scheduler.
//
// In practice, we 'd have a coro_ctx used by the scheduler we could always return to -- and the scheduler should check if its the only registered(ready or stopped)  coro
// If not, it should be able to exit
static void coroInit()
{
        // We have stored(reg, arg) in the stack here so that when control was transfered over to this trampoline, we 'd know
        // what to do. There are probably better ways to do this, but for now, this will do fine.
        // XXX: this doesn't work if -O1 or higher, because the stack structure is different than what we expect here.
        // Should be easy to fix (store func,arg elsewhere)
        // http://eli.thegreenplace.net/2011/09/06/stack-frame-layout-on-x86-64/
        void (*func)(void *);
        void *arg;

        asm volatile(
            "movq 16(%%rbp), %%rax\n"
            "movq %%rax, %0\n"
            "movq 8(%%rbp), %%rax\n"
            "movq %%rax, %1\n"
            : "=m"(func), "=m"(arg)::"%rax");

#if __GCC_HAVE_DWARF2_CFI_ASM && __amd64
        asm(".cfi_undefined rip");
#endif

        func(arg);

        // Your trampoline should e.g coro_resume(&corosSchedulerCtx);

        // We can never return from here
        // Just schedule next, if none, schedule the 'main'(scheduler) coro
        // We are supposed to _DESTROY_ coro (return to free list)
        // We could have used coro_transfer() before we do that, but for performance reasons, use
        // coro_resume() which doesn't need to save state
        // e.g coro_resume(&corosSchedulerCtx);
        assume_unreachable();
}

void coro_init(coro_ctx *ctx, void (*func)(void *), void *arg, coro_stack *const coroStack)
{
        coro_init_with_trampoline(ctx, func, arg, coroStack, coroInit);
}

// How many guard stack pages to use. Accesses to those (below the stack) will result in segment violations (as opposed to being silently ignored)
#define GUARD_PAGES_CNT 2

bool coro_stack::alloc(size_t hint, const uint8_t protect)
{
        static const auto pageSize = sysconf(_SC_PAGESIZE);

        // Round up to multiple of page size
        size = (hint + pageSize - 1) & ~(pageSize - 1);
        if (protect)
        {
                // if we are going to use posix_memalign(), size must be multiple of page size
                // for now, we are just going to use mmap()
                //
                // 'waste' some guard pages below the stack -- protect it so that rogue accesses will be caught
                stack = mmap(nullptr, size + GUARD_PAGES_CNT * pageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

                if (stack == MAP_FAILED)
                        return false;
                else
                {
#if GUARD_PAGES_CNT > 0
                        // can't touch this
                        mprotect(stack, GUARD_PAGES_CNT * pageSize, PROT_NONE);
#endif

                        flags = 3;
#ifdef VALGRIND_SUPPORT
                        valgrindId = VALGRIND_STACK_REGISTER((char *)stack, (char *)stack + (size + GUARD_PAGES_CNT * pageSize));
#endif
                        stack = ((char *)stack) + GUARD_PAGES_CNT * pageSize;
                }
        }
        else
        {
                stack = malloc(size);

                if (!stack)
                        return false;

                flags = 1;
#ifdef VALGRIND_SUPPORT
                valgrindId = VALGRIND_STACK_REGISTER((char *)stack, (char *)stack + size);
#endif
        }

        return true;
}

void coro_stack::release()
{
#ifdef VALGRIND_SUPPORT
        VALGRIND_STACK_DEREGISTER(valgrindId);
#endif

        if (flags)
        {
                if (flags & 2)
                {
                        static const auto pageSize = sysconf(_SC_PAGESIZE);

                        munmap((char *)stack - GUARD_PAGES_CNT * pageSize, size + GUARD_PAGES_CNT * pageSize);
                }
                else
                        free(stack);
        }
}

bool coro_stack::set(void *const p, const size_t s)
{
        stack = p;
        size = s;
#ifdef VALGRIND_SUPPORT
        valgrindId = VALGRIND_STACK_REGISTER(stack, size);
#endif
        return true;
}

asm(
    ".text\n"
    ".globl coro_transfer\n"
    "coro_transfer:\n"

#if __amd64
    "pushq %rbp\n"
    "pushq %rbx\n"
    "pushq %r12\n"
    "pushq %r13\n"
    "pushq %r14\n"
    "pushq %r15\n"
    "movq %rsp, (%rdi)\n" // store RSP in coro_ctx->sp  (first function arg. stored in RDI)

    // http://eli.thegreenplace.net/2011/09/06/stack-frame-layout-on-x86-64/
    // when coro_transfer() is invoked, the runtime makes sure that the IP is stored at RBP + 8
    // so when we resume here on in coro_resume, right after the popped 6 regs, we can expect
    // that we'll return to where the coro we switched to yielded when it _invoked_ either coro_transfer() or coro_resume()
    // (because when it called either of those, the IP at that point was set in the stack)
    //
    // coro_init_with_trampoline() is important because it setups the stack for new coros(not the 'main' context)
    // so that the 6 dummy regs are pushed in (we will pop them here and in coro_resume() impl.)
    // and that when coro_resume() or coro_transfer() returns the first time it's invoked for the coroutine, it will jmp to the trampoline

    "movq (%rsi), %rsp\n" // load RSP from coro_ctx->sp (second function arg. stored in RSI)
    "popq %r15\n"
    "popq %r14\n"
    "popq %r13\n"
    "popq %r12\n"
    "popq %rbx\n"
    "popq %rbp\n"

#if 0
	// ret pops IP and jmps to it
	// we may as well just ret here directly
    "popq %rcx\n"
    "jmpq *%rcx\n"
#else
    "ret\n"
#endif

    ".globl coro_resume\n"
    "coro_resume:\n"
    "movq (%rdi), %rsp\n"
    "popq %r15\n"
    "popq %r14\n"
    "popq %r13\n"
    "popq %r12\n"
    "popq %rbx\n"
    "popq %rbp\n"

    "ret\n"

#else
#error arch not supported
#endif
    );

void coro_init_with_trampoline(coro_ctx *ctx, void (*func)(void *), void *arg, coro_stack *const coroStack, void (*trampoline)())
{
        ctx->sp = (void **)(coroStack->size + (char *)coroStack->stack);
        ctx->sp = (void **)(uintptr_t(ctx->sp) & -16L); // 16-byte aligned down
        *--ctx->sp = (void *)abort;                     // in case trampoline returns, it will return to abort (trampoline should never return)

        *--ctx->sp = (void *)trampoline;
        // 6 regs tracked
        // coro_resume() will pop those 6 registers, and then it will jump to our trampoline (ret will pop the IP and jmp to it)
        ctx->sp -= 6;
}

#pragma mark Fibers
void Fibers::FiberTrampoline()
{
#if __GCC_HAVE_DWARF2_CFI_ASM && __amd64
        asm(".cfi_undefined rip");
#endif

        auto &s = scheduler;

        s.cur->L();
        s.on_fiber_exit();

        assume_unreachable();
}

Fibers::Scheduler::~Scheduler()
{
        for (auto &it : freeStacks)
                it.release();

        for (auto it : freeFibers)
                it->~fiber();

#ifdef HAVE_FIBER_PRIOS
        for (auto &it : ready)
        {
                while (it.next != &it)
                {
                        auto f = switch_list_entry(fiber, list, it.next);

                        switch_dlist_del(&f->list);
                        f->stack.release();
                        f->~fiber();
                }
        }
#else
        while (ready.next != &read)
        {
                auto f = switch_list_entry(fiber, list, ready.next);

                switch_dlist_del(&f->list);
                f->stack.release();
                f->~fiber();
        }
#endif
}

Fibers::fiber *Fibers::Scheduler::get_fiber()
{
        auto *const f = freeFibers.size() ? freeFibers.Pop() : allocator.construct<fiber>();

        if (freeStacks.size())
                f->stack = freeStacks.Pop();
        else
        {
                f->stack.alloc(4096);
                // only need to initialize if its a new fiber
                // for reused fibers, we always switch_dlist_del_and_reset() on it when its scheduled anyway
                switch_dlist_init(&f->list);
        }

        ++aliveCnt;
        return f;
}

void Fibers::Scheduler::put_fiber(fiber *const f)
{
        f->L = nullptr;

        freeFibers.push_back(f);
        if (freeStacks.size() > 32)
        {
                // Frugal
                f->stack.release();
        }
        else
                freeStacks.push_back(f->stack);

        --aliveCnt;
}

void Fibers::Scheduler::make_ready(fiber *const f)
{
#ifdef HAVE_FIBER_PRIOS
        const auto prio = f->prio;

        switch_dlist_insert_before(&f->list, &ready[prio]);
        readyMask |= 1u << prio;
#else
        switch_dlist_insert_before(&f->list, &ready);
#endif

        ++readyCnt;
}

Fibers::fiber *Fibers::Scheduler::next_ready()
{
// O(1) scheduler
#ifdef HAVE_FIBER_PRIOS
        const auto idx = SwitchBitOps::TrailingZeros(readyMask);
        auto &l = ready[idx];
#else
        auto &l = ready;
#endif

        auto f = switch_list_entry(fiber, list, l.next);

        switch_dlist_del_and_reset(&f->list);

#ifdef HAVE_FIBER_PRIOS
        if (switch_dlist_isempty(&l))
                readyMask &= ~(1u << idx);
#endif

        --readyCnt;
        return f;
}

void Fibers::Scheduler::resume(fiber *const f)
{
        // The main scheduler may need to Resume() a bunch of suspended coros, and then just switch control to the next ready
        // or, maybe we 'd like to immediately transfer control here regardless of the priority.  For now, we just
        // expect the scheduler implementation to YieldFromMain() at the end of the run loop iteration
        make_ready(f);
}

void Fibers::Scheduler::suspend()
{
#ifdef HAVE_FIBER_PRIOS
        if (!readyMask)
#else
        if (!readyCnt)
#endif
        {
#ifdef HAVE_FIBER_TS
                cur->ts = Timings::Microseconds::Tick();
#endif

                ++ctxSwitches;
                coro_transfer(&cur->ctx, &mainCoro);
        }
        else
        {
                auto *const t = cur;

                cur = next_ready();
#ifdef HAVE_FIBER_TS
                cur->ts = t->ts = Timings::Microseconds::Tick();

#endif
                ++ctxSwitches;
                coro_transfer(&t->ctx, &cur->ctx);
        }
}

void Fibers::Scheduler::yield()
{
#ifdef HAVE_FIBER_PRIOS
        if (!readyMask)
#else
        if (!readyCnt)
#endif
        {
                // Just us!
                return;
        }
        else
        {
                auto *const t = cur;

                cur = next_ready();

#ifdef HAVE_FIBER_TS
                cur->ts = t->ts = Timings::Microseconds::Tick();
#endif

                make_ready(t);

                ++ctxSwitches;
                coro_transfer(&t->ctx, &cur->ctx);
        }
}

void Fibers::Scheduler::yield_from_main(coro_ctx *const ctx)
{
#ifdef HAVE_FIBER_PRIOS
        if (readyMask)
#else
        if (readyCnt)
#endif
        {
                cur = next_ready();

#ifdef HAVE_FIBER_TS
                cur->ts = Timings::Microseconds::Tick();
#endif

                ++ctxSwitches;
                coro_transfer(ctx, &cur->ctx);
        }
}

void Fibers::Scheduler::yield_from_main()
{
        yield_from_main(&mainCoro);
}

void Fibers::Scheduler::await_fiber(fiber *const f)
{
        auto *const prev = cur;

        f->awaits = cur;
        coro_init_with_trampoline(&f->ctx, nullptr, f, &f->stack, FiberTrampoline);

        if (!readyCnt)
        {
                // Fast-Path: no other ready coros, schedule this here
                cur = f;
        }
        else
        {
                make_ready(f);
                cur = next_ready();
        }

#ifdef HAVE_FIBER_TS
        prev->ts = cur->ts = Timings::Microseconds::Tick();
#endif

        ++ctxSwitches;
        coro_transfer(&prev->ctx, &cur->ctx);
}

void Fibers::Scheduler::on_fiber_exit()
{
        if (auto *const f = cur->awaits)
                make_ready(f);

        // we 'll release cur coro, even though we could keep it around just so until
        // we coro_transfer() from it
        // we 'll just release and use coro_resume() directly to the next
        put_fiber(cur);

        ++ctxSwitches;
#ifdef HAVE_FIBER_PRIOS
        if (!readyMask)
#else
        if (!readyCnt)
#endif
        {
                // Ok, switch back to main
                // Simple is beautiful
                coro_resume(&mainCoro);
        }

        cur = next_ready();

#ifdef HAVE_FIBER_TS
        cur->ts = Timings::Microseconds::Tick();
#endif

        coro_resume(&cur->ctx);
}

#ifdef EXAMPLE
#pragma mark Example
static uint32_t switchesCnt{0};
static coro_ctx ctx1, ctx2;
uint64_t start;

static void T1()
{
#if __GCC_HAVE_DWARF2_CFI_ASM && __amd64
        asm(".cfi_undefined rip");
#endif

        for (;;)
        {
                if (++switchesCnt == 20'000'000)
                {
                        // About 58-62 million/second on a Xeon 2Ghz
                        const auto duration = Timings::Microseconds::Since(start);

                        Print("Took ", duration_repr(duration), " ", dotnotation_repr(1.0 / Timings::Microseconds::ToSecondsF(duration) * 20'000'000), "/sec\n");
                        _exit(1);
                }
                coro_transfer(&ctx1, &ctx2);
        }
        assume_unreachable();
}

static void T2()
{
#if __GCC_HAVE_DWARF2_CFI_ASM && __amd64
        asm(".cfi_undefined rip");
#endif
        for (;;)
        {
                if (++switchesCnt == 20'000'000)
                {
                        const auto duration = Timings::Microseconds::Since(start);

                        Print("Took ", duration_repr(duration), " ", dotnotation_repr(1.0 / Timings::Microseconds::ToSecondsF(duration) * 20'000'000), "/sec\n");
                        _exit(1);
                }
                coro_transfer(&ctx2, &ctx1);
        }

        assume_unreachable();
}

static constexpr bool trace{false};
int main(int argc, char *argv[])
{
#if 0
	{
		uint32_t sum{0};
		uint8_t mask = 1<<4;
		const auto b= Timings::Microseconds::Tick();

		for (volatile uint32_t i{0}; i != 20'000'000; ++i)
			sum+=SwitchBitOps::TrailingZeros(mask);
	
		Print("Took ", duration_repr(Timings::Microseconds::Since(b)), "\n");
		return 0;
	}
#endif

#if 0
	{
		// Benchmark context switching
		coro_stack s1, s2;

		s1.Alloc(4096);
		s2.Alloc(4096);

		start = Timings::Microseconds::Tick();
		coro_init_with_trampoline(&ctx1, nullptr, nullptr, &s1, T1);
		coro_init_with_trampoline(&ctx2, nullptr, nullptr, &s2, T2);

		coro_resume(&ctx1);

		exit(0);
	}
#endif

        const auto b = Timings::Microseconds::Tick();

        Fibers::scheduler.schedule([]() {
                Fibers::scheduler.schedule([]() {
                        for (uint32_t i{0}; i != 65536 / 2; ++i)
                        {
                                if (trace)
                                        Print("NOW at ", ansifmt::color_green, ptr_repr(Fibers::scheduler.cur), ansifmt::reset, "\n");

                                Fibers::scheduler.yield();
                        }
                });

                for (uint32_t i{0}; i != 65536 / 2; ++i)
                {
                        if (trace)
                                Print("NOW at ", ansifmt::color_brown, ptr_repr(Fibers::scheduler.cur), ansifmt::reset, "\n");

                        Fibers::scheduler.yield();
                }

        });

        Fibers::scheduler.yield_from_main();

        Print("Took ", duration_repr(Timings::Microseconds::Since(b)), " ", dotnotation_repr(Fibers::scheduler.ctxSwitches), "\n");

        if (trace)
                Print("Now back to main\n");

        return 0;
}
#endif
