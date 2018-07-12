+++
title = "Asynchronous models in C++"
tags = ["concurrency", "asynchronous"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2018-07-12T20:45:24+03:00
draft = true
+++

Lately there are many discussions in programming community in general and in c++ community specifically which asynchronous programming model to use. Those discussions are not new and actually originate to 70'es when actor model together with futures and continuations was proposed. Cooperative multi tasking models starting with coroutines were introduced even before 70-es and were followed up by green threads.

Each one of the asynchronous models were popularized in mainstream languages. For example, coroutines are used in Python (yield) and Lua. Continuations and futures are used extensively in Java. Callback based actor models are used in C and Javascript. Golang and Erlang are using green threads. C++ community did not take an official stance on the matter with very minimal support of futures in c++11.

I would like to share my opinion on what I think will be the best direction for asynchronous model in C++ by reviewing 2 libraries: [Seastar](http://seastar.io/) and [Boost.Fiber](https://boost.org/doc/libs/1_67_0/libs/fiber/doc/html/index.html).


<!--more-->

# Seastar
Seastar is a powerhorse behind very efficient Nosql database [ScyllaDB](https://scylladb.com) and is developed by a very talanted team of world class developers. Seastar is a framework that adopts [continuation-passing style](https://en.wikipedia.org/wiki/Continuation-passing_style) (CSP) of writing fully asynchronous code. The framework itself has been proven very efficient, in fact it allows to respond with sub-millisecond latencies (even at 99nth percentile) and scale linearly in number of cores. It has been used already in [few projects](http://seastar.io/seastar-applications/) and gave consistently superior performance compared to existing alternatives.

Seastar fully shows the true potential of a hardware it utilizes, has tons of features to cover many possible scenarios and it's a perfect candidate to analyze how futures and continuations look in C++.

## 1. Code (un)-clearness.

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
It has a flow that is common to all continuation passing frameworks. It indents to right with each continuation. It's really hard to apprehend and it's hard to see which block can access what variables.
In addition if exceptions are thrown it's not immediately clear where to put the catch handler: it can be thrown from code running "now" or coming from the continuation lambda. Both cases require different handling.
Continuation chaining has implicit interfaces that are hard to define using classical interfaces. Lambdas and autos are coming to rescue but those do not make the code much clearer.

## 2. Object life management.

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

Lets see another example. Suppose we have a moveable-only class `pipe` that has an asynchronous method `read`. After reading the previous paragraph we happily write the following code:

```cpp
future<> (pipe p) {
  return p.read(16, [p = std::move(p)] (buffer b) {});
}
```

Unfortunately this code will crush since C++ is *deliberetely* does not define the order at which the lambda capture happens relative to other statements. So what may (and does) happen is that the compiler first moves the object `p` inside the lambda and then calls `p.read(..)` on already undefined object.
The workaround to this problem is to use `shared_ptr` to allow copy semantics and pay attention to captures that move objects.

As you can see using CSP in C++ is not trivial matter. Unfortunately, it's not Seastar to blame but comes from the fact how CSP interacts with C++ core language rules.

Seastar team tries to overcome those difficulties by providing building blocks for most common syncronization patterns and for giving idiomatic way on how to use their framework. The disadvantage of this approach is that many of their utilities hide memory allocations and it is anti-idiomatic way of building C++ framework. Also, that does not really solve the code clearness issue.

There are also Seastar specific problems: it requires that all the code inside will follow specific rules which basically deny an integration with any other open source library that uses system calls.
