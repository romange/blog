+++
title = "How to serialize integers into memory"
tags = ["c++", "code generation", "programming"]
categories = []
description = ""
images = []
date = 2017-09-26T22:46:30+03:00
draft = false
+++

Here is the analysis of a recent bug happenned to me recently which caused me to think that
the compiler has a bug (or that ["These are wrong bees"](https://www.youtube.com/watch?v=PIuE5J9dfAo)).
Consider the code below. We copy 64 integers into a properly allocated destination buffer and yet,
if compiled with `-O3` switch this code crahes with segfault!.

<!--more-->

{{<code "storing_ints.cpp">}}



You can see it yourself by running it with [g++ on x64 architecture](http://rextester.com/FJSB11478).
A quick session with gdb shows that it fails on the instruction [`movaps`](http://www.felixcloutier.com/x86/MOVAPS.html). g++ is smart enough to perform vectorized
optimization and copy 2 64 bit integers at once. Unfortunaltely `movaps` requires that its destination memory operand would be 16 byte aligned but `SerializeTo` can not possibly know
whether `dest` is aligned or not. To my surprise g++ does not generate any preamble code that handles possible unalignment issues the way I would expect him to, nor it decides to fallback to instructions that handle unligned words. Is g++ in his rights?

g++ is correct to generate this code because we broke some basic rules and this code has undefined behavior (UB) due to incorrect cast
`reinterpret_cast<uint64_t*>(dest)`. See "Type Aliasing" paragraph [here](http://en.cppreference.com/w/cpp/language/reinterpret_cast). According to these "strict aliasing" rules we can not cast from `uint8_t*` to `uint64_t*` and if we do it causes an UB.
We made g++ believe that it can perform aligned write due to our cast to `uint64_t*`.

Before we talk about solution, lets check how common such mistake is.
Many google projects, for example, have the [following macro](https://github.com/search?utf8=%E2%9C%93&q=UNALIGNED_STORE64+reinterpret_cast&type=Code) `#define UNALIGNED_STORE64(_p, _val) (*reinterpret_cast<uint64_t *>(_p) = (_val))` for x64 bit architecture.
The name of the macro eloquently shows that it's meant for storing integer into possibly unaligned address. There are millions other appearences of `reinterpret_cast` in github and I estimate that vast part of them do not follow strict aliasing rules.

So how do we copy an integer into specific memory address? We could, of course copy it byte by byte
but that's slow. The only acceptable solution I know of is to use `memcpy(dest, src, sizeof(uint64))`. In optimized mode, the compiler recognizes `memcpy` and replaces it with specific CPU instructions according to the target architecture. In case of uint64_t on x64 it translates to a single `mov` instruction. While it maybe suboptimal (it's still possible to use vectorized instructions) it's nethertheless correct.

I think it's a bit sad that currently C++ language does not have a dedicated tool in the language that explicitly allows storing a computer word in memory and instead we need to rely on
an external function but that's state of matters at this moment.

If you know any other standard way of performing this task correctly please tell me.



