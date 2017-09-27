+++
title = "How to serialize integers into memory."
tags = ["c++", "code generation", "programming"]
categories = []
description = ""
menu = ""
# banner = "https://www.gravatar.com/avatar/4a203a48eb1cdb1b8c47d008f0c139c9?s=100&d=identicon"
images = []
date = 2017-09-26T22:46:30+03:00
draft = true
+++

Here is the analysis of a recent bug happenned to me recently which caused me to think that
the compiler has a bug (or that ["These are wrong bees"](https://www.youtube.com/watch?v=PIuE5J9dfAo)).
Consider the code below. We copy 64 integers into a properly allocated destination buffer and yet,
if compiled with `-O3` switch this code crahes with segfault!.

<!--more-->

{{<code "storing_ints.cpp">}}



You can see it yourself by running it with [g++ on x64 architecture](http://rextester.com/FJSB11478).
A quick session with gdb shows that it fails on the instruction [`movaps`](http://www.felixcloutier.com/x86/MOVAPS.html). g++ is smart enough and bold enough to perform vectorized
optimization and copy 2 64 bit integers at once. Unfortunaltely `movaps` requires that its destination memory operand would be 16 byte aligned but `SerializeTo` can not possibly know
whether `dest` is aligned or not. To my surprise g++ does not generate any preamble code that handles possible unalignment issues the way I would expect him to, nor it decides to fallback to instructions that handle unligned words. Is g++ in his rights?

g++ is correct to generate this code because we broke some basic rules and this code has undefined behavior (UB) due to incorrect cast
`reinterpret_cast<uint64_t*>(dest)`. See "Type Aliasing" paragraph [here](http://en.cppreference.com/w/cpp/language/reinterpret_cast). According to these "strict aliasing" rules we can not cast from `uint8_t*` to `uint64_t*` and if we do it causes an UB.
We made g++ believe that it can perform aligned write due to our cast to `uint64_t*`.

Before we talk about solution, lets check how common such mistake is.
Many google projects, for example, have the [following macro](https://github.com/search?utf8=%E2%9C%93&q=UNALIGNED_STORE64+reinterpret_cast&type=Code) `#define UNALIGNED_STORE64(_p, _val) (*reinterpret_cast<uint64_t *>(_p) = (_val))` for x64 bit architecture.
The name of the macro eloquently shows that it's meant for storing integer into possibly unaligned address. There are millions other appearences of `reinterpret_cast` in github and I estimate that vast part of them do not follow strict aliasing rules.

So how do we copy an integer into specific memory address? We could, of course copy it byte by byte
but that's slow.



