+++
title = "A prelude to analysis of Redis memory-store "
tags = ["redis", ]
categories = []
description = ""
menu = ""
banner = "banners/1200px-Redis_Logo.svg.png"
images = []
date = 2021-11-28T09:46:19+03:00
draft = false
+++

During the last 13 years, Redis has become a truly ubiquitous memory store that has won the hearts
of numerous dev-ops and software engineers. Indeed, according to [StackOverflow survey in
2021](https://insights.stackoverflow.com/survey/2021#databases), Redis is the most loved database for the 5th time in a row and is at the top of
[db-engines](https://db-engines.com/en/ranking/key-value+store) ranking, way before the next contestant.
But how well does Redis utilizes modern hardware systems?
Will it stay competitive in a few years without reinventing itself?

<!--more-->

To understand choices behind Redis design, I have been reading [the old posts](http://oldblog.antirez.com/) of Salvatore @antirez \- the creator of Redis. Before I begin, I want to add that I have tremendous respect for Salvatore and for how much he achieved by just being a talented and authentic software programmer
(even though [he does not want to be remembered as such](http://antirez.com/news/133)).

Based on his notes and GitHub discussions, I identified the following architectural principles in Redis:

1. **Single-threaded architecture**\
Redis development started a few years after Memcached. By that time, Memcached, the predecessor of Redis,
has already been a mature system that has been used and supported by large, highly
technological companies like Facebook and Twitter. It used multi-threaded architecture
to scale its I/O performance vertically within a single node.
Remarkably, antirez decided against this architecture and adopted the single-threaded design instead.
Specifically, Redis utilizes a single thread that manipulates its
main in-memory dictionary. antirez has defended this approach many times with the following arguments:
     * Redis cares very much about latency and adopts share-nothing architecture to control its tail
       and average latencies.
     * Most of the CPU spent on system-cpu handling I/O and not on userland cpu handling Redis data-structures.
       Therefore, the upside of parallelization is limited anyway.
     * Pipelining of requests can increase throughput by order of magnitude.
     * On the other hand, multi-threading adds complexity. Quoting antirez:
        "...Slower development speed to achieve the same features. Multi-thread programming is hard...
        In a future of cloud computing, I want to consider every single core as a computer itself..."
     * Vertical scale has physical limit anyway, therefore horizontal scaling (aka Redis cluster)
       is the way to scale.

2. **Strong consistency guarantees when reading and writing from master node**\
   Redis provides linearizability guarantees as long as you read and write to the same machine.
   That means that all mutations appear to be executed atomically in a sequential order that
   is consistent with a global order (wall-clock time) of non-overlapping operations.
   In practice, this is a direct consequence of utilizing a single thread for applying
   datastore operations atomically.

3. **Simple is beautiful, code is a poem**\
   Salvatore's preference is towards simple solutions. He expressed his attitude to coding in his
   [Redis manifesto](http://oldblog.antirez.com/post/redis-manifesto.html). Redis resides in
   a single coherent codebase without much reliance on third-party projects. One example of this principle
   is that Salvatore crafted all Redis core data structures with the Redis dictionary being probably the most
   prominent one. Redis dictionary powers the main hash table that stores
   all the entries in the Redis datastore. It has been also used for some Redis value types like sets
   and maps. Redis dictionary uses a classic chained hashtable implementation with
   some improvements to allow dynamic rehashing.


In addition to the principles above, Redis maintains unique design goals that differentiate it
from, say, a disk-based database. I believe that if we list Redis design goals by priority, it will be:
1. Low latency
2. High throughput
3. Memory efficiency
4. High availability
5. Strong consistency guarantees
6. Durability

## Retrospective
I will not argue with the tremendous popularity of the Redis memory store. It seems that Salvatore's
decision to go for simplicity and deliver features quickly - paid off: today Memcached is a niche
system, and the vast majority of software stacks use Redis. However, I do claim that it is possible to
**vastly** improve reliability, performance and cost-efficiency metrics of Redis-like memory store that
follows similar design goals but with different architectural principles.

In my next posts, I am going to go over Redis design principles described above and show how a different
architecture, if aligned better with modern hardware systems, could provide, what I think
a disruptive change to in-memory datastores.