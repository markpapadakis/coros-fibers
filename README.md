# coros-fibers
Coroutines/Fibers implementation for x86.
See [Coroutines and Fibers. why and When](https://medium.com/software-development-2/coroutines-and-fibers-why-and-when-5798f08464fd#.qz59082w0) article for information.

This is an initial release, and it depends on [our](http://phaistosnetworks.gr/) Switch framework to compile. I will remove those dependecies later and will provide a better example than what's available in coroutines.cpp, and will also add a proper Makefile.

I am pushing this here so that I will be motivated to do this right sometime soon :-)

You can do whatever you want with this code - distribute it freely.

## Performance
For x86-64, 15 instructions are needed(see `coro_transfer` implementation) for a context switch. 
We could do better by disregarding the ABI and tracking fewer registers (and making sure we know what we are doing and the caveats), although I believe its unnecessary, all things considered. We could also improve the scheduler (maybe disable support for priorities and time tracking -- see macros).
On a Xeon 2Ghz system, this implementation will execute 21million context switches/second when using the fibers framework. That's fast enough.
For just context switching (use of `coro_transfer` and `coro_restore`), on the same system, we get 58 to 62 million context switches. This is very fast.


## API
There are two APIs. The lower-level coroutines API (just `coro_transfer` and `coro_restore` for context switching, and two `coro_init` variants, along with 2 more functions for coro stacks ) and the higher level Fibers scheduler API. Please see coroutines.h

## Purprose and Benefits
Appropriate use of coroutines/fibers and context switching when needed(for example, when the current task is about to block, or has been monopolizing the CPU for too long) can have a great impact in the performance of your system, because it can help implement fair scheduling and improve the utilization of your OS thread.
They are also extremely easy to use. You can just schedule new coros, yield and await for them with a single function call, no need for continuations or FSMs, or other means of tracking state. Every coroutine has its own stack, and everything just works as expected.
Please see the article for more.
