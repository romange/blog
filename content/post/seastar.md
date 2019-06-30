+++
title = "Seastar - Asynchronous C++ framework"
tags = ["concurrency", "asynchronous", "seastar"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2018-07-12T20:45:24+03:00
draft = false
+++

Lately, there are many discussions in the programming community in general and in c++ community in particular on how to write efficient asynchronous code. Many concepts like futures, continuations, coroutines are being discussed by c++ standard
committee but not much progress was made besides very minimal support of C++11 futures.

On the other hand, many mainstream programming languages progressed quicker and adopted asynchronous models
either into a core language or popularized it via standard libraries. For example, coroutines are used in Python (yield) and Lua.
Continuations and futures are used extensively in Java. Golang and Erlang are using green threads.
Callback based actor models are used in C and Javascript. Due to a lack of official support for
asynchronous programming in C++, the community introduced ad-hoc frameworks and libraries that allow writing asynchronous code in C++.

I would like to share my opinion on what I think will be the best direction for asynchronous models in C++ by reviewing two
prominent frameworks: [Seastar](http://seastar.io/) and [Boost.Fiber](https://boost.org/doc/libs/1_67_0/libs/fiber/doc/html/index.html). This (opinionated) post reviews Seastar.


<!--more-->

# Seastar
Before I talk about Seastar I want to describe the problem that asynchronous programming models solve
the way I see it:

**Given a single execution thread** and a program flow consisting of potentially blocking task
how can we write a program that fully utilizes this thread. "fully utilizes" in this context means that the execution thread
won't sleep as long as there are any (CPU) tasks that could be performed.

Please note that asynchronous programming is not the same as multi-threading even though
multi-threading might provide a workaround to a similar problem: **Given a single CPU core** how can we
write a program that fully utilizes this CPU. Asynchronous programming here is discussed only in
the context of a single thread even though many models provide a complete solution for both asynchronous
programming and multi-threaded environments.

Seastar is a power-horse behind a very efficient Nosql database [ScyllaDB](https://scylladb.com) and
is developed by a very talented team of world-class developers. Seastar is a framework that adopts [continuation-passing style](https://en.wikipedia.org/wiki/Continuation-passing_style) (CSP) of writing fully asynchronous code.

CSP programming solves asynchronous programming by breaking the program flow into CPU-only tasks
that should not block and IO requests. Each io request returns a future object describing the result.
Program flow is extended by appending to a future a possible continuation - either a cpu-task or another io-request.
The goal of this post is not to review or explain CSP but to check whether CSP can be applied naturally inside C++
based on my experience with Seastar.

Seastar framework itself has proven to be very efficient. In fact, it allows responding to requests
with sub-millisecond latencies (even at 99nth percentile) and scale linearly in the number of cores.
It has been used already in [a few projects](http://seastar.io/seastar-applications/) and
gave consistently superior performance compared to the existing alternatives.

Seastar shows the true potential of the hardware it utilizes. It has lots of pre-built constructs
to cover many synchronization scenarios and I think it's a perfect candidate to analyze how futures
and continuations code might look in C++.

## Scopes and execution context
Right from the start, it was hard for me to get used to the unique style of writing CSP.
The problem is common to Java futures as well, not just to C++ or Seastar.

Consider, for example, the following code taken from Seastar's wiki page.
It starts a listener loop on port 1234 that accepts server connections.
For each accepted connection it writes a response into its socket and then closes it.

```cpp
seastar::future<> service_loop() {
    seastar::listen_options lo;
    lo.reuse_address = true;

    return seastar::do_with(seastar::listen(seastar::make_ipv4_address({1234}), lo),
            [] (auto& listener) {
        return seastar::keep_doing([&listener] () {
            return listener.accept().then(
                [] (seastar::connected_socket s, seastar::socket_address a) {
                    auto out = s.output();
                    return seastar::do_with(std::move(s), std::move(out),
                        [] (auto& s, auto& out) {
                            return out.write(canned_response).then([&out] {
                                return out.close();
                });
            });
            });
        });
    });
}
```

The code has a notable flow that is common to all continuation-passing frameworks.
It indents to right with each continuation or lambda. There are lots of lambda functions
and it's very hard to follow which block can access what variables.
Every continuation or asynchronous function breaks the execution context and it becomes non-trivial effort
to reason about the flow of the program.

Now consider `seastar::keep_doing`. It is a pre-built routine that accepts an (a)synchronous function
as an argument and it allows running this function in the infinite but preemptable loop.
Why do we need such a function?  Suppose our goal is to schedule `do_smthing_async()` time after time
sequentially. The function itself might block due to I/O, but we do not want to stall the execution thread.
Since we want to write fully asynchronous code, we can not just write `while (true) { do_smthing_async(); }`
because then we are trapped inside the loop. Instead `keep_doing` uses "futurized" recursion to run the code and schedule it again (simplified below):

```
  // AsyncAction is a callback returning seastar::future<> des an asynchronous computation.
  template seastar::future<> keep_doing(AsynchAction action) {
    return action.then([action] { return keep_doing(action);});
  }
```

The code above is very simplified and the real Seastar code can handle many loops constructs with
synchronous or asynchronous functions. In addition, Seastar does optimizations to improve
the latency of asynchronous operations when possible. As a result, `keep_doing` provides meta
construct to tell Seastar engine about asynchronous state-machine describing a loop.
The execution thread is not blocked we schedule the loop.
Eventually, when we schedule the initial state of the program the main thread
will enter the execution loop which will start unwinding the  state that was queued into the engine and execute
its asynchronous commands.

As a result, C++ language is used to create and connect Seastar data-structures that tell the Seastar framework
how to run functions instead of actually running them. Such style differs greatly from classic C++ programming.
Some native C++ constructs work differently with such flow. For example, if exceptions are thrown
in CSP code it's not immediately clear where to put the catch handler:
it can be thrown from the code currently scheduling the io request, or alternatively,
it might be thrown from the continuation lambda when it runs. Both cases require different handling. As a result continuation chaining has implicit interactions that are not tracked via core language.

## Object life management.
If following the execution context or changing the mindset for scheduling asynchronous dependencies
can be hard in C++, then tracking object lifecycles can be a real hell.

Unlike in Python, Java, Go, a c++ programmer is required to think about object lifecycle and with CSP
it's much harder even with naive looking cases. Consider the following code:

```cpp
seastar::future<uint64_t> slow_size(file f) {
    return seastar::sleep(10ms).then([f] {
        return f.size();
    });
}
```

`f.size()` is implemented as an asynchronous operation because it might need to
read file attributes from the disk i.e. to block on IO. The lambda that captures `f` exits immediately
after issuing an asynchronous call `f.size()`. The asynchronous code that is gonna run by the framework requires `f` to exist, of course, but `f` captured by lambda and is gonna be destroyed after the lambda exits.
In order to prolong the object's life we are advised to add `finally([f] {})` which
serves as an object scoped guard for Seastar flow.

```cpp
seastar::future<uint64_t> slow_size(file f) {
    return seastar::sleep(10ms).then([f] {
        return f.size().finally([f]{});
    });
}
```

Let's see another example. Suppose we have a moveable-only class `pipe` that has an asynchronous method
`future<> read(size_t, AsyncFunc f)`. `AsyncFunc` is a function that is expected to receive a future
object containing buffer contents upon successful read. With lessons learned from the previous example,
we might write the following code to keep `p` alive while lambda runs.

```cpp
future<> (pipe p) {
  return p.read(16, [p = std::move(p)] (auto future_buf) { handle(future_buf); ... });
}
```

Unfortunately, this code causes "undefined behavior" and most likely will crash because C++ does
not define the order evaluation of lambda capture and function call inside the same execution statement.
So what may (and does) happen is that the compiler first moves the object `p` into the lambda and
then calls `p.read(..)` on already undefined object `p`.
The workaround to this problem is to use `shared_ptr` to allow copy semantics and pay attention to captures that move objects.

As you can see using CSP in C++ is not a trivial matter and I showed only a few issues a programmer needs to think about when he uses Seastar. Unfortunately, it's not Seastar to blame - it comes from the fact that C++ is not designed to describe CSP flows.

Seastar team tries to overcome those difficulties by providing building blocks for most common
synchronization patterns and for giving an idiomatic way on how to use their framework.
One of the disadvantages of this approach is that many of their utilities hide memory allocations,
which is by itself anti-idiomatic to C++. For example, their solution to the problem above is
using `do_with` construct that allows binding the scope of moveable objects to their asynchronous operations
but hides the allocation of its helper objects. As a result, the framework becomes complicated and hard to learn.

In addition, the constructs like these still do not solve the core problem - future chaining looks
confusing and unnatural in C++.

To summarize, Seastar is a very efficient framework but requires total commitment to it right from the start.
It allows you to describe fully asynchronous flows but requires lots of attention - debugging the problems might be hard.
If you decide using it, you probably won't be able to integrate it well with other open-source libraries,
especially with those that use synchronous system-calls, synchronization mechanisms, and multi-threading.
Despite the fact that having the CSP style of writing asynchronous code can be very tedious in C++,
hard to reason and error-prone it's still possible.

In my next post I will go over [Boost.Fiber](https://boost.org/doc/libs/1_67_0/libs/fiber/doc/html/index.html)
framework and will explain how it solves asynchronous programming but retains the original style of writing C++ programs.
