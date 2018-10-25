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
asynchronous programming. My personal opinion is that this style does not work well in C++ and in
general harder to read.

In this post I would like to cover Fiber-based approach.

<!--more-->

In this post I will use `boost.fibers` library in all my examples.

Fibers or [green threads](https://en.wikipedia.org/wiki/Green_threads) or *cooperative threads* are
similar to regular threads but with few important differences:

1. Fibers can not switch between system threads and they are usually pinned to a specific thread.
2. Fibers do not perform contexts-switches without explicit directive and only one fiber can be active
(running) in the thread. The switch is performed by context (and stack) switching between different
fibers. Therefore, things that are considered thread-unsafe can be perfectly safe with fibers:
{{< highlight cpp "linenos=table" >}}
  int shared_state = 0;
  auto cb = [&] { shared_state += 1;};

  fiber fb1(cb);
  fiber fb2(cb);
  fb1.join();
  fb2.join();
{{< / highlight >}}
Both fibers read-modify-write the same unprotected variable, but because each one of them must run in
turns inside the calling thread data race does not happen and `shared_state` is changed sequentially.
The order of the changes above is undefined.

3. The main difference between fibers and coroutines is that fibers require a scheduler
which decides what active fiber is called next according to some scheduling policy.
4. Fibers are user-land creatures, their creation does not involve any system calls and
thus is more efficient than of threads (by factor of 100).
5. Standard thread-blocking calls block will block fibers as well and in fact, will cancel any possible advantages
of fibers. For example,
{{< highlight cpp "linenos=table" >}}
  fiber fb1([] { sleep(1); });

  fiber fb2([] {
    this_fiber::yield();
    do_something_in_background();
  });

  fb1.join();
  fb2.join();
{{< / highlight >}}

in this snippet we start 2 fibers and make sure that `fb1` will progress first by yielding the execution
from `fb2`. However, `fb1` makes system call to `sleep()`, and there is no mechanism in place to
switch the execution back from `fb1` to `fb2`. Therefore the thread will stall for 1 second and only
after `fb1` finishes it ill resume with `fb2`.

The same true with synchronization:
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
Here, like in the previous, example, `fb2` yields to allow `fb1` to start running first. `fb1` blocks
and waits for `fb2` to signal, but since we use regular mutex and condition variables
no context switching is done and we have deadlock.

To coordinate between fibers we need fiber specific mechanisms and boost.fibers provides them.
We have `fibers::mutex`, `fibers::conditional_variable`, `fibers::future<>` and `fibers::promise<>`.
In addition, we have (fiber) mpmc blocking queue `fibers::buffered_channel` similarly to
golang's channels.


