+++
title = "Performance analysis of Redis store"
tags = ["redis", ]
categories = []
description = "Unpopular opinion"
menu = ""
banner = ""
images = []
date = 2021-10-13T09:46:19+03:00
draft = true
+++

During the last 13 years, Redis became a truly ubiquitous memory store that has won the hearts
of numerous dev-ops and software engineers. Indeed, according to [StackOverflow survey in
2021](https://insights.stackoverflow.com/survey/2021#databases), Redis is the most loved database for the 5th time in a row and is at the top of
[db-engines](https://db-engines.com/en/ranking/key-value+store) ranking, way before the next contestant.
But how *really* well does Redis fit modern hardware systems? Will it stay competitive in 5 years without reinventing itself?

<!--more-->

It's really interesting to read [the old posts](http://oldblog.antirez.com/) of Salvatore @antirez
\- the author of Redis. One can learn what were his design goals at that time and what has driven
him to develop Redis. Before I begin, I want to add that I have tremendous respect for Salvatore and
for how much he achieved by just being a talented and authentic software programmer
(even though [he does not want to be remembered as such](http://antirez.com/news/133)).


Based on his notes and GitHub discussions, I identified the following design decisions in Redis.

1. **Single-threaded architecture**\
Redis development started few years after Memcached. By that time, Memcached, the predecessor of Redis,
has already been a mature system and been used and supported by large, highly
technological companies like Facebook and Twitter. It used multi-threaded architecture
to scale its I/O performance vertically within a single node.

Remarkably, antirez decided against this architecture and adopted the single-threaded design instead.
Specifically, Redis utilizes a single thread that accesses its
main K/V in-memory dictionary. antirez has defended this approach many times with the following arguments:
     * Redis cares very much about latency and adopts share-nothing architecture to control the tail
       and average latencies.
     * Most of the CPU spent on system-cpu handling I/O and not on user-cpu handling Redis data-structures.
       Hence the potential of parallelizing datastore operations is limited.
     * On the other hand multi-threading adds complexity:
        "...Slower development speed to achieve the same features. Multi-thread programming is hard...
        In a future of cloud computing I want to consider every single core as a computer itself..."
     * Pipelining can speed-up processing by order of magnitude!
     * Vertical scale has limitations anyway, Redis cluster is the way to scale!
2. **Using fork() for writing and replicating snapshots**\
Redis uses OS provided `fork()` call to achieve snapshot isolation consistency during its
serialization procedure while still handling write traffic. Linux OS issues Copy-on-Write
for memory pagesthat were touched by the parent process while child process continues reading from immutable snapshot of process memory.


Appendix:
https://medium.com/@john_63123/redis-should-be-multi-threaded-e28319cab744
https://twitter.com/antirez/status/1110973404226772995
http://antirez.com/news/85
https://medium.com/@john_63123
https://twitter.com/kellabyte/status/1111380252398280704


