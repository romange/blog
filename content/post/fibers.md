+++
title = ""
tags = []
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2018-10-25T12:59:52+03:00
draft = true
+++

In my [post about Seastar]( {{< ref "seastar.md" >}}) we've covered continuations style
asynchronous programming. My personal opinion is that this style does not fit well and is hard to work with in C++. In this post I would like to cover Fiber-based approach.

<!--more-->

In this post I will use `Boost.Fibers` library in all my examples.

Fibers or [green threads](https://en.wikipedia.org/wiki/Green_threads) or *cooperative threads* are similar to regular threads but with few important distinctions:

1. Fibers can not switch between system threads and they are usually pinned to a specific thread.
2. Fibers do not perform contexts-switches without explicit directive and only one fiber can be active (running) in the thread at the same time. The switch is performed by context (and stack) switching between different fibers and must be initiated by user code. Therefore, we may write asynchronous, unprotected but safe code as long as our data access is single threaded. For example,

{{< highlight cpp "linenos=table" >}}
  int shared_state = 0;
  auto cb = [&] { shared_state += 1;};

  fiber fb1(cb);
  fiber fb2(cb);

  // Possible CPU computations in the middle. Could change shared_state as well.
  // ....
  fb1.join();
  fb2.join();
{{< / highlight >}}

Both fibers read-modify-write the same unprotected variable. The fiber scheduler might switch the execution from main fiber to `fb1` or `fb2` either during fiber creation or during the `join()` calls. In any case each one of the fibers will run in turns until completion because they do not have any "interrupt" points inside. Therefore `shared_state` will be changed sequentially. The order of the apllied changes is undefined in the example above.

3. The main difference between fibers and coroutines is that fibers require a scheduler
which decides which active fiber is called next according to a scheduling policy. Coroutines, on the other hand, are more explicit and pass the execution to a specified point in code.
4. Fibers are user-land creatures, their creation, nor synchronization does not involve any system calls and thus they are more efficient than threads (by factor of 100).
5. Standard thread-blocking calls will block fibers as well and, in fact, will cancel any possible advantages of fibers. For example,

{{< highlight cpp "linenos=table" >}}
  fiber fb1([] { sleep(1); });

  fiber fb2([] {
    this_fiber::yield();
    do_something_in_background();
  });

  fb1.join();
  fb2.join();
{{< / highlight >}}

in this snippet we start 2 fibers and make sure that `fb1` will progress first by yielding the execution from `fb2`. However, `fb1` makes direct system call to `sleep()`, and there is no mechanism in place to switch the execution back from `fb1` to `fb2`. Therefore the thread will stall for 1 second and only after `fb1` finishes it ill resume with `fb2`.

The same true with regular synchronization primitives:
{{< highlight cpp "linenos=table" >}}
  bool state = false;
  std::mutex mu;
  std::conditional_variable cv;

  fiber fb1([&] {
    std::unique_lock<std::mutex> lock(mu);
    cv.wait(lock, [&] { return state; });
  });

  fiber fb2([] {
    this_fiber::yield();
    state = true;
    cv.notify_one();
    do_something_in_background();
  });

  fb1.join();
  fb2.join();
{{< /highlight >}}
Here, like in the previous example, `fb2` yields to allow `fb1` to start running first. `fb1` blocks and waits for `fb2` to signal, but since we use regular mutex and condition variables
no context switching is done ending up with a deadlock.

To coordinate between fibers we need fiber specific mechanisms and boost.fibers provides them.
We have `fibers::mutex`, `fibers::conditional_variable`, `fibers::future<>` and `fibers::promise<>`. In addition, we have (fiber) mpmc blocking queue `fibers::buffered_channel` similarly to golang's channels. Those routines recognize when fiber stalling is needed and they call fiber scheduler to resume with the next active fiber. This allows asynchronous execution that does not stall the running thread until no-active fibers are left and the system is blocked on I/O or some other signal. That means we must use fiber-aware code every time we use IO or call an asynchronous event.


## Seastar comparison
If you remember we reviewed Seastar's `keep_doing` method of repeating futurized action in the loop. With fibers it becomes much simpler and familiar:
{{< highlight cpp "linenos=table" >}}

int keep_going = true;
int iteration = 1;
fibers::mutex mu;
fibers::conditional_variable cv;

auto cb = [&] {
  while (keep_going) {
    do_something_fiber_blocking();
    ++iteration;
    cv.notify_on();
  }
};

fiber fb1(cb);
fiber fb2([&] {
  {
    std::unique_lock<fibers::mutex> lock(mu);
    cv.wait(lock, [&] { return iteration > 5;});
  }
  keep_going = false;
});

fb1.join();
fb2.join();
{{< /highlight >}}

Our loop is just a regular loop but its asynchronous action should block the calling fiber when it does not have result yet. When `do_something_fiber_blocking` blocks the execution switches to any other active fiber. It may be that `fb2` is active and did not run yet, in that case it will run and block at `cv.wait` line. fb1 will iterate and wake fb2 on each iteration.
But only when the condition is fullfilled `fb2` will set `keep_going` to false. Please note that both `keep_going` and `iteration` variables are unprotected. As you can see, writing asynchronous code with fibers is not very hard as long as its asynchronous code is "fiber-friendly".

Another example from the previous post:
{{< highlight cpp "linenos=table" >}}
size_t slow_size(file f) {
  this_fiber::sleep(10ms);
  return f.size();
}
{{< /highlight >}}

As in the previous post, we stall the calling fiber for `10ms` and then call IO function `f.size()`. Unlike with Seastar  the flow is just a regular C++ flow with the destinction that the thread "switches" to other execution fibers at every "stalling" point like "sleep" and "f.size(). In case of "sleep" the framework instructs the scheduler to suspend the calling fiber and to resume itself after 10ms. In case of `f.size()` the developer of `file` class is responsible to suspend the fiber upon the asynchronous call to IO device that should bring that file metadata and wake the fiber back once the call has been completed. The synchronization can be done easily using `fibers::mutex` and `fibers::conditional_variable` constructs as long as the underlying IO interface allows completions callbacks.

Unlike with Seastar the fiber-based code is much simpler and object ownership issues are less hassle. Seastar model assumes infinite threads of executions and even when we want synchronous continuity we must use continuations to synchronize between consecuative actions. With Fibers model we assume synchronous flow by default, and if we want to spawn parallel executions we need to launch new fibers. Therefore, amount of parallelism with Fibers is limited by number of launched fibers, while Continuations Model provides infinitely large parallelism by building depency graph of futures and continuations. I believe that even in case of very sophisticated asynchronous processing most flows look synchronous with just few junctions where we want to launch many asynchronous actions in parallel. Therefore I believe that Fiber based model is more convenient in general and in C++ especially due to various language restrictions and lack of automatic garbage collection like.

It's not totally fair to compare `boost.fibers` to [Seastar]( {{< ref "seastar.md" >}}) because Seastar provides high level framework for asynchronous execution as well as RPC, HTTP, networking and events interfaces under the same hood. `Boost.Fibers` on other hand is low-level library focused only on Fibers. `Boost.Asio` and `Boost.Fibers` can provide somewhat similar functinality to Seastar. Unfortunately there is not much material in the internet on how to use efficiently those libraries together. That's why I released [gaia](fobar) framework that provides
high level mechanisms to build high-performance backends.
