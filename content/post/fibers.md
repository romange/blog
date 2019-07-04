+++
title = "Introduction to fibers in c++"
tags = ["c++", "asynchronous", "fibers", "reactive"]
categories = []
description = "A small introduction to fiber-based programming model based on Boost.Fibers library"
menu = ""
banner = ""
images = []
date = 2018-12-15T11:59:52+03:00
+++

In my [post about Seastar]( {{< ref "seastar.md" >}}) we've covered continuations style
asynchronous programming. My personal opinion is that this style is hard to work with in C++.
In this post I would like to cover alternative, Fiber-based approach.
I will use `Boost.Fibers` library in all my examples.

Fibers, or [green threads](https://en.wikipedia.org/wiki/Green_threads), or *cooperative threads* are similar to regular threads but with few important distinctions:

1. Fibers can not move between system threads and they are usually pinned to a specific thread.

2. Fibers scheduler does not perform implicit contexts-switches: a fiber must explicitly give up
its control over its active execution and only one fiber can be active (running) in the thread at the same time.
The switch is performed by changing the active context and the stack to another fiber.
The old-stack is preserved until the old fiber resumes its execution. As a result,
we may write asynchronous, unprotected but safe code as long as our data access is single threaded.

3. The main difference between fibers and coroutines is that fibers require a scheduler which decides which active fiber is called next according to a scheduling policy. Coroutines, on the other hand, are simpler - they pass the execution to a specified point in code.

4. Fibers are user-land creatures, their creation or context switches betwen them does not involve kernel,
 and thus they are more efficient than threads (by factor of 100).

5.  Standard thread-blocking calls will block fibers and, in fact, will cancel any possible advantages of fibers.

### Accessing data from multiple fibers.
The snippet below demonstrates point (2) above.

{{<highlight cpp "linenos=table">}}
  int shared_state = 0;
  auto cb = [&] { shared_state += 1;};

  fiber fb1(cb);
  fiber fb2(cb);

  // Possible CPU computations in the middle. Could change shared_state as well.
  // ....
  fb1.join();
  fb2.join();
{{</highlight>}}

Both fibers read-modify-write the same unprotected variable. The fiber scheduler might switch
the execution from the main fiber to `fb1` or `fb2` either during fiber creation or during the `join()` calls.
In this case, I explicitly launched both fibers without switching the execution,
i.e. the main fiber continued running passed constructors until `fb1.join()` call.
During `fb1.join()` call the main fiber suspends itself until `fb1` finishes running.
However, it's not guaranteed which fiber will run next: `fb1` or `fb2` -
this depends on the scheduler policy. In any case, each one of spawned fibers will run in
turns until the completion because they do not have any "interrupt" points inside their run functions.
As a result, `shared_state` will be changed sequentially.
We did not show what is the scheduler policy in this example, therefore
the order of the applied changes to `shared_state` is unknown here.

### Calling thread blocking functions is bad for fibers
This snippet demonstrates point(5) above.

{{< highlight cpp "linenos=table" >}}
  fiber fb1([] { sleep(1); });

  fiber fb2([] {
    this_fiber::yield();
    do_something_in_background();
  });

  fb1.join();
  fb2.join();
{{< / highlight >}}

Here we start 2 fibers and make sure that `fb1` will progress first by yielding (switching) the execution from `fb2`.
Once `fb1` starts running it makes a direct system call to `sleep()`. Sleep is OS function
that is not aware of our fibers - they are user-land.
Therefore the whole thread will stall for 1 second and only when `fb1` finishes, it ill resume with `fb2`.

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

To coordinate between fibers we need fiber-specific mechanisms which boost.fibers provides.
We have `fibers::mutex`, `fibers::conditional_variable`, `fibers::future<>` and `fibers::promise<>`.
Also, we have (fiber) mpmc blocking queue `fibers::buffered_channel`, which is similar to golang's channels.
Those constructs recognize when a fiber is stalled and call fiber scheduler to resume with the next active fiber.
This enables asynchronous execution which does not block the running thread until no active fibers are left and the system is blocked on I/O or some other signal. That means we must use fiber-cooperative code every time we use I/O or call an asynchronous event.


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

Our loop is just a regular loop but its asynchronous action should block the calling fiber
when it does not have the result yet. When `do_something_fiber_blocking` blocks, the execution switches to any other active fiber. It may be that `fb2` is active and did not run yet. In that case, it will run and block at `cv.wait` line.
fb1 will iterate and wake fb2 on each iteration.
But only when the condition is fulfilled `fb2` will set `keep_going` to false.
Please note that both `keep_going` and `iteration` variables are unprotected.
As you can see, writing asynchronous code with fibers is simpler as long as you leverage
fiber properties to your advantage.

Another example from the previous post: fetching the file size asynchronously using the handler object.
{{< highlight cpp "linenos=table" >}}
size_t slow_size(file f) {
  this_fiber::sleep(10ms);
  return f.size();
}
{{< /highlight >}}

As in the previous post, we stall the calling fiber for `10ms` and then call IO function `f.size()`.
Unlike with Seastar, the flow is just a regular C++ flow with the distinction that the thread "switches"
to other execution fibers at every "stalling" point like "sleep" and "f.size().

With `sleep()`, the framework instructs the scheduler to suspend the calling fiber
and to resume itself after 10 milliseconds.

With `f.size()`, the developer of `file` class is responsible to suspend the fiber upon the
asynchronous call to IO device that should bring that file metadata and wake the fiber back once
the call has been completed. The synchronization can be done easily using `fibers::mutex` and
`fibers::conditional_variable` constructs as long as the underlying IO interface allows completion callbacks.

The fiber-based code is simpler than futures-based code and object ownership issues are less of a hassle.
The `file` object does not go out of scope before the asynchronous operations finish because
the calling fiber preserves the call-stack like with the classic programming models.
Seastar model assumes infinite threads of executions and even when we want synchronous continuity
we must use continuations to synchronize between consecutive actions.
With Fibers model we assume synchronous flow by default, and if we want to spawn parallel executions
we need to launch new fibers. Therefore, the amount of parallelism with fibers is controlled by number
of launched fibers, while Continuations Model provides infinitely large parallelism
by building the dependency graph of futures and continuations.

I believe that even in the case of very sophisticated asynchronous processing most flows look
synchronous with just a few branches where we want to launch many asynchronous actions in parallel.
Therefore I believe that Fiber-based model is more convenient in general and in C++ especially
due to various language restrictions and lack of automatic garbage collection.

It's not totally fair to compare `boost.fibers` to [Seastar]( {{< ref "seastar.md" >}}) because Seastar provides high level framework for asynchronous execution as well as RPC, HTTP, networking and events interfaces under the same hood.
`Boost.Fibers` on other hand is low-level library focused only on Fibers.
`Boost.Asio` and `Boost.Fibers` can provide somewhat similar functinality to Seastar.
Unfortunately there is not much material in the internet on how to use efficiently those libraries together.
That's why I released [GAIA](https://github.com/romange/gaia) framework that provides high level
mechanisms to build high-performance backends. I will continue talking about GAIA in my next posts.
