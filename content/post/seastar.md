+++
title = "Seastar - Asynchronous C++ framework"
tags = ["concurrency", "asynchronous", "seastar"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2018-07-12T20:45:24+03:00
draft = true
+++

Lately there are many discussions in programming community in general and in c++ community in particular on how to write efficient asynchronous code. Those discussions are not new and actually originate to 70'es when actor centric model was proposed together with futures and continuations. Cooperative multi-tasking models like coroutines were introduced even before 70-es and later were followed up by "green" or cooperative threads.

Each one of those asynchronous models were popularized in most mainstream languages. For example, coroutines are used in Python (yield) and Lua. Continuations and futures are used extensively in Java. Callback based actor models are used in C and Javascript. Golang and Erlang are using green threads. C++ stands out by not taking an official stance on the matter with just minimal support of futures in c++11.

I would like to share my opinion on what I think will be the best direction for asynchronous models in C++ by reviewing two modern frameworks: [Seastar](http://seastar.io/) and [Boost.Fiber](https://boost.org/doc/libs/1_67_0/libs/fiber/doc/html/index.html). This post reviews Seastar.


<!--more-->

# Seastar
Seastar is a powerhorse behind very efficient Nosql database [ScyllaDB](https://scylladb.com) and is developed by a very talanted team of world class developers. Seastar is a framework that adopts [continuation-passing style](https://en.wikipedia.org/wiki/Continuation-passing_style) (CSP) of writing fully asynchronous code. The framework itself has been proven very efficient, in fact it allows to respond with sub-millisecond latencies (even at 99nth percentile) and scale linearly in number of cores. It has been used already in [few projects](http://seastar.io/seastar-applications/) and gave consistently superior performance compared to the existing alternatives.

Seastar fully shows the true potential of a hardware it utilizes. It has lots of pre-built constructs to cover many synchronization scenarios and it's a perfect candidate to analyze how futures and continuations code could look in C++. And, unfortunately, it does not look good.

Right from the start it was hard for me to get used to the unique style of writing CSP.
The problem is common to Java futures as well, not just to C++ or Seastar.

Consider, for example, the following code taken from Seastar's wiki page:

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

It has a flow that is common to all continuation passing frameworks. It indents to right with each continuation or lambda. It's really hard to apprehend and it's hard to see which block can access what variables.

Consider `seastar::keep_doing`. It is a pre-built routine that allows running a infinite but preemptable loop inside Seastar that calls an (a)synchronous function that depicts the body of the. Since we want to write fully asynchronous code we can not just write `while (true) { do_smthing_async(); }` because then the running thread will be stuck in this loop forever. Instead we use "futurized" recursion to run the code and schedule it again (simplified below):

```
  // AsyncAction is a callback returning seastar::future<> des an asynchronous computation.
  template seastar::future<> keep_doing(AsynchAction action) {
    return action.then([action] { return keep_doing(action);});
  }
```

It's very simplified and the real Seastar code can handle for loops, moveable or synchronous functions. In addition Seastar tries to do optimizations to improve latency of asynchronous operations when possible.

If exceptions are thrown in CSP code it's not immediately clear where to put the catch handler: it can be thrown from the code currently running, or, alternatively will be thrown from the continuation lambda once it runs. Both cases require different handling. As a result continuation chaining has implicit interfaces that are hard to define using classical interfaces. Lambdas and autos are coming to rescue but those do not make the code much clearer.

## Object life management.

Unlike in Python, Java, Go, a c++ programmer is required to think about object lifecycle and with CSP
it's much harder even with naive looking cases. Consider the following code:

```cpp
seastar::future<uint64_t> slow_size(file f) {
    return seastar::sleep(10ms).then([f] {
        return f.size();
    });
}
```

`f.size()` is implemented as an asynchronous operation because it requires reading file attributes from the disk. The lambda that captures `f` exits immediately after issuing an asynchronous call captured by Seastar's framework. The asynch code that is gonna run requires `f` to be alive, of course, but `f` captured by lambda and is gonna be destroyed after the lambda exits. In order to prolong object's life we are advised to add the following 'hack' to push the ownership further. `finally([f] {})` serves as an object guard for CSP flows.

```cpp
seastar::future<uint64_t> slow_size(file f) {
    return seastar::sleep(10ms).then([f] {
        return f.size().finally([f]{});
    });
}
```

Lets see another example. Suppose we have a moveable-only class `pipe` that has an asynchronous method
`future<> read(size_t, AsyncFunc f)`. When `AsyncFunc` receives a buffer containing future object upon successful reading. With lessons learned from the previous paragraph we confidently write the following code:

```cpp
future<> (pipe p) {
  return p.read(16, [p = std::move(p)] (auto future_buf) { handle(future_buf); ... });
}
```

Unfortunately this code will crash because C++ is *deliberetely* does not define the order at which the lambda capture of our handler function happens relative to other statements. So what may (and does) happen is that the compiler first moves the object `p` into the lambda and then calls `p.read(..)` on already undefined object `p`.
The workaround to this problem is to use `shared_ptr` to allow copy semantics and pay attention to captures that move objects.

As you can see using CSP in C++ is not a trivial matter. Unfortunately, it's not Seastar to blame - it comes from the fact that CSP interacts badly with C++ core language rules.

Seastar team tries to overcome those difficulties by providing building blocks for most common syncronization patterns and for giving an idiomatic way on how to use their framework. The disadvantage of this approach is that many of their utilities hide memory allocations which is by itself anti-idiomatic to C++. For example, their solution to the problem above is using `do_with` utility that allows to bind the scope of moveable objects to their asynchronous operations.

```cpp
TODO: See do_with(T&& t)
```

Also, the constructs like these still do not make the code much clearer - future chaining looks unnatural in C++.

There are also Seastar specific problems: it requires that all the code inside will follow specific rules which basically deny an integration with any other open source library that uses system calls.

To summarize, Seastar is a very efficient framework that requires total commitment to it right from the beginning. It allows you to describe fully asynchronous flows but requires lots of attention and debugging the problems might be hard. If you decide to use it, you probably won't be able to integrate it well with other open source libraries, especially with those that use synchronous system-calls, synchronization mechanisms and multi-threading.
