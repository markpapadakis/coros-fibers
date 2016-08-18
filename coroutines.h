/*
 * 	coroutines.h
 *
 *	See https://medium.com/software-development-2/coroutines-and-fibers-why-and-when-5798f08464fd#.hsfh18d5f
 *
 *	The libc get/make/swap-context() API is somewhat more powerful, for makecontext() can setup as trampoline a function
 * 	that accepts arbitrary number of arguments. The downside is complexity.
 *	Originally, this library provided alternative implementations to those APIs. It was only storing and restoring the regs ABI stipulates we should track, doing away with
 * 	signals mask and floating points regs. It was working fine.
 *  
 * 	I don't really need that - I only need is a single argument, though it should be pretty straight forward to support arbitrary number of arguments
 *	like makecontext() does, but the overhead and complexity required to implement that do not justify pursuing this change.
 * 	See https://github.com/lattera/glibc /sysdeps/unix/sysv/linux/x86_64/ for glibc implementation specifics.
 *
 *	Instead, what makes more sense and is infinitely simpler is to use a static thread_local for the trampoline's context.
 *
 * 	We could also support more platforms/arcs, and other means of control transfer. 
 * 	As explained by https://github.com/clibs/coro/coro.h, other approaches include:
 * 	- use of SUSv2's get/set/swap/makecontext API. Supported by some systems, and slow
 * 	- use of SUSv2's setjmp/longjmp and sigaltstack APIs to specify a stack for a signal handler's stack. Much slower to create coro than ucontext APIs, but somewhat faster to switch between them. 
 *		We can and should use the alternative _setjmp/_longjmp instead (will not incur the system call to save and restore the siganls mask) - which results in much higher performance.
 * 		This is supported by all Unix flavors
 * 	- variant for glibc <= 2.1, for Linux, which is very fast, but requires older glibc. This comes down to use of a different use of sigsetjmp() and siglongjmp() APIs
 * 	- use of the PThreads API. This is very slow and better be avoided
 *  	If you need to support multiple archs, use the excellent https://github.com/clibs/coro
 *
 *	I suppose I could just provide support to other unix flavors via getcontext()/makecontext()/setcontext() for initialization and setjmp()/longjmp() for switches which is faster
 *	until assembly optimized implementations to coro_transfer() and coro_resume() can be implemented. (see SeaStar thread_context design)
 *
 *
 * 	PERFORMANCE
 *	On a Xeon at 2Ghz, we get about 21million context switches/second. This is far more than we would need, assuming appropriate use of coroutines/fibers and context switching
 *	where it makes sense(e.g before blocking ops, or when a coro has been running for too long), which means there will be an infrequent need for context switching, it should
 *	be the 'solution' to achieving very high performance.
 *	
*/
#pragma once
#if !defined(__amd64) || !defined(__linux)
// TODO: support i386-linux(trivial). Support for other unix flavors via makecontext(), setjmp() and longjmp(). (that's what Seastar does it)
#error Unsupported platform/arch
#endif
#define VALGRIND_SUPPORT 1
#include <switch.h>
#ifdef VALGRIND_SUPPORT
#include <valgrind/valgrind.h>
#endif
#include <switch_bitops.h>
#include <switch_ll.h>
#include <switch_mallocators.h>
#include <switch_vector.h>

#pragma mark Coroutines
struct coro_stack
{
        void *stack;
        size_t size;
        uint8_t flags{0};
#ifdef VALGRIND_SUPPORT
        int valgrindId;
#endif

        bool alloc(size_t hint, const uint8_t protect = false);
        void release();
        bool set(void *const p, const size_t s);
};

struct coro_ctx
{
        void **sp; // must be offset 0 in coro_ctx; see coro_transfer and coro_resume impl.
};

// We absolutely need to make sure the compiler won't inline those (e.g -O2 or higher)
extern "C" {
extern void __attribute__((__noinline__, __regparm__(2)))
coro_transfer(coro_ctx *prev /* rdi */, coro_ctx *next /* rsi */);

extern void __attribute__((__noinline__, __regparm__(1)))
coro_resume(coro_ctx *ctx /* rdi */);
};

// http://wiki.osdev.org/System_V_ABI
// functions are called using the CALL instruction, which pushes the address of the next instruction to the stack, and then jumps to the operand
// functions return to the caller using the RET instruction, which pops a value from the stack and jumps to it.
//
// The stack is 16-byte aligned just before the call instruction is called
//
// Functions preserve rbx, rsp, rbp, r12, r13, r14 and r15; while rax, rdi, rsi, rdx, rcx, r8, r9, r10, r11 are scratch regs.
// The return value is stored in the rax reg, or if it is a 128-bit value, then the higher 64-bits go in rdx
//
// Optionally, functions push rbp such that the caller-return-rip is 8 bytes above it, and erp to the addres of the saved rbp. this allows iterating the existing stack frames.
void coro_init_with_trampoline(coro_ctx *ctx, void (*func)(void *), void *arg, coro_stack *const coroStack, void (*trampoline)());
void coro_init(coro_ctx *ctx, void (*func)(void *), void *arg, coro_stack *const coroStack);

#pragma mark Fibers
// We need both different prios and time tracking, but let's make this tunable here
#define HAVE_FIBER_PRIOS 1 // Support different priorities
#define HAVE_FIBER_TS 1    // track coroutines timestamps(running since, ended at)

namespace Fibers
{
        void FiberTrampoline();

        struct fiber
        {
                coro_ctx ctx;
                coro_stack stack;

#ifdef HAVE_FIBER_PRIOS
                // fibers of priority X will be scheduled before those of priority > X
                // 0 is the highest prio, 7 is the lowest
                uint8_t prio;
#endif

#ifdef HAVE_FIBER_TS
                uint64_t ts; // if currently running, when it was scheduled, otherwise, when it stopped running
#endif

                switch_dlist list; // in ready list for its priority
                fiber *awaits;     // if set, another fiber is waiting for this fiber to complete
                std::function<void()> L;
        };

        // this is go's scheduler implementation
        // https://github.com/golang/go/blob/master/src/runtime/proc.go
        // findrunnable() is very interesting. Will GC if needed.
        // It first checks the local runq. If it is empty, it then checks the global run queue.
        // It then check netpoll, and then randomly steals from other Ps (see M, N, P)
        struct Scheduler
        {
              public:
                fiber *cur{nullptr};

              private:
#ifdef HAVE_FIBER_PRIOS
                uint8_t readyMask{0};
#endif

              public:
                uint32_t readyCnt{0};
                // For performance measurements
                uint32_t ctxSwitches{0};

              private:
#ifdef HAVE_FIBER_PRIOS
                switch_dlist ready[8];
#else
                switch_dlist ready;
#endif

              public:
                uint32_t aliveCnt{0}; // includes suspended and not runnable count
                simple_allocator allocator;
                Switch::vector<fiber *> freeFibers;
                Switch::vector<coro_stack> freeStacks;

              private:
                fiber *get_fiber();

                void put_fiber(fiber *const f);

                // Makes fiber `f` runnable. That is, it will be eventually schedulef by trasnfering execution control to it
                void make_ready(fiber *const f);

                // Here you could roll your own which would e.g also consider
                // fibers that haven't had a chance to run for too long, or always schedule a particular fiber regardless
                // of priorities first, etc. Most likely should be made virtual so
                // that other scheduler implementations can do what's needed
                fiber *next_ready();

              public:
                inline auto current() const noexcept
                {
                        return cur;
                }

                Scheduler()
                {
#ifdef HAVE_FIBER_PRIOS
                        for (auto &it : ready)
                                switch_dlist_init(&it);
#else
                        switch_dlist_init(&ready);
#endif
                }

                ~Scheduler();

                template <class F>
                void schedule(F &&l, const uint8_t prio = 4)
                {
                        auto *const f = get_fiber();

                        Drequire(f);
                        f->L = std::move(l);
                        // We are using our own trampoline, just so that we we 'll save one un-necessary call
                        // from the standard coroInit trampoline to another. We also don't need to provide a func.ptr
                        coro_init_with_trampoline(&f->ctx, nullptr, f, &f->stack, FiberTrampoline);
                        f->awaits = nullptr;

#ifdef HAVE_FIBER_PRIOS
                        f->prio = prio;
#endif

                        make_ready(f);
                }

                // Puts current fiber to sleep.
                // Something will need to wake it up later. Typically, you 'd obtain curent active fiber (e.g scheduler.Cur() call)
                // and have something later invoke scheduler.Resume() on it, so that it can be rescheduled.
                //
                // For example, if you are about to perform some expensive I/O op, you may want to do it off thread and then resume when
                // you are done, e.g
                // {
                // 	auto cur = scheduler.current();
                //
                //	WorkOffThread(cur);
                // 	suspend();
                // }
                void suspend();

                void resume(fiber *);

                // Yield control to another runnable fiber, but also make this current fiber runnable
                // so that it will get a chance to run again as soon as possible based on the scheduler semantics.
                void yield();

                // Convenience method. Schedule a fiber and immediately yield
                template <class F>
                void yield(F &&f, const uint8_t prio = 4)
                {
                        schedule(std::move(f), prio);
                        yield();
                }

                // This should be invoked in the execution context of a 'scheduler', that is, not in the context
                // of a fiber (though a scheduler can of course be implemented as a fiber).
                // Control will be transfered to the next runnable fiber.
                void yield_from_main(coro_ctx *const ctx);

                void yield_from_main();

                // Make `f` ready, and schedule the next runnable
                // It will suspend the current fiber and will then schedule the next runnable. Once
                // the scheduled `f` completes, the now suspended fiber will be made runnable again and potentially immediately rescheduled
                //
                // See "An Introduction to C++ Coroutines by James McNellis" https://youtu.be/YYtzQ355_Co?t=770
                // where he is demonstrating that
                // {
                // 	auto result = co_await expression;
                // }
                // is translated to
                // {
                //	auto &&__a = expression;
                //
                //	if (!__a.await_ready())
                //	{
                //		__a.await_suspend(coroutine-handle);
                // 		// .. suspend/resume points ..
                // 	}
                // 	auto result = __a.await_resume();
                // }
                //
                //
                // Conversely, co_return y is translated to ( see https://youtu.be/YYtzQ355_Co?t=2727 )
                // {
                // 	__promise.return_value(x);
                //	goto __final_suspend_label;
                // }
                // and co_yield z to
                // {
                // 	co_await __promise.yield_value(z);
                // }
                void await_fiber(fiber *const f);

                template <class T>
                void await(T &&l, const uint8_t prio = 4)
                {
                        auto *const f = get_fiber();

#ifdef HAVE_FIBER_PRIOS
                        f->prio = prio;
#endif
                        f->L = std::move(l);
                        await_fiber(f);
                }

                // Whenever a fiber function returns(i.e fiber has exited)
                // this is invoked.
                void on_fiber_exit();
        };

        extern thread_local coro_ctx mainCoro;
        extern thread_local Scheduler scheduler;
}
