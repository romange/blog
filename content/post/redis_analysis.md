+++
title = "A prelude to Redis memorystore analysis"
tags = ["redis", ]
categories = []
description = ""
menu = ""
banner = "banners/1200px-Redis_Logo.svg.png"
images = []
date = 2021-10-13T09:46:19+03:00
draft = true
+++

During the last 13 years, Redis has become a truly ubiquitous memory store that has won the hearts
of numerous dev-ops and software engineers. Indeed, according to [StackOverflow survey in
2021](https://insights.stackoverflow.com/survey/2021#databases), Redis is the most loved database for the 5th time in a row and is at the top of
[db-engines](https://db-engines.com/en/ranking/key-value+store) ranking, way before the next contestant.
But how well does Redis utilizes modern hardware systems?
Will it stay competitive in few years without reinventing itself?

<!--more-->

It is really interesting to read [the old posts](http://oldblog.antirez.com/) of Salvatore @antirez
\- the author of Redis. One can learn about his design goals were at that time and what has driven
him to develop Redis. Before I begin, I want to add that I have tremendous respect for Salvatore and
for how much he achieved by just being a talented and authentic software programmer
(even though [he does not want to be remembered as such](http://antirez.com/news/133)).


Based on his notes and GitHub discussions, I identified the following design principles in Redis.

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
     * Most of the CPU spent on system-cpu handling I/O and not on user cpu handling Redis data-structures.
       Therefore, the upside of parallelization is limited anyway.
     * Pipelining of requests can increase throughput by an order of magnitude.
     * On the other hand, multi-threading adds complexity. Quoting antirez:
        "...Slower development speed to achieve the same features. Multi-thread programming is hard...
        In a future of cloud computing, I want to consider every single core as a computer itself..."
     * Vertical scale has physical anyway, horizontal scaling (aka Redis cluster) is the way to scale!

2. **Using fork() for writing snapshots**\
Redis uses OS provided `fork()` to achieve snapshot isolation consistency for its
serialization procedures while handling the write traffic. It uses OS ability to
transparently issue Copy-on-Write of memory pages that are touched by the parent process
while the child process continues reading from the immutable snapshot of process memory.
While the serialization continues, every mutated memory page in the parent process doubles
physical memory usage until the child process finishes serialization and exits.

3. **Memory-only stable state replication**\
   Redis stable state replication uses a dedicated memory ring buffer to keep the latest mutations.
   Redis keeps `k` last mutations in his ring buffer (replication log).
   Newly added contents of this replication log are pushed into sockets of each one of the slave replicas.
   If a slave replica lags behind the master farther than the replication buffer can provide,
   then the slave must disconnect and perform a full resync. Unrelated to Redis replication design,
   Redis can write AOF - a variation of journal with tunable durability guarantees. Interestingly,
   Redis does not use AOF in order to extend the replication window beyond its ring buffer capacity.

4. **Strong consistency guarantees when reading and writing from master node**\
   Redis provides linearizability guarantees as long as you read and write to the same machine.
   That means that all mutations appear to have executed atomically in some sequential order that
   is consistent with a global order (wall-clock time) of non-overlapping operations.
   In practice, this is a direct consequence of utilizing a single thread for applying
   datastore operations atomically.

5. **Simple is beautiful, code is a poem**\
   Salvatore's preference is towards simple solutions. He expressed his attitude to coding in his
   [Redis manifesto](http://oldblog.antirez.com/post/redis-manifesto.html). Redis resides in
   a single coherent codebase without much reliance on third-party projects.
   Salvatore crafted all the core data structures with Redis dictionary being probably the most
   important one. Redis dictionary is used as the main hash table that stores
   all the entries in the datastore. It has also been used for some Redis value types like sets
   and hsets. Redis dictionary uses a classic chained hashtable implementation with
   some improvements to allow dynamic rehashing.


I believe that if we list Redis design goals by priority, it will be:
1. Low latency
2. High throughput
3. Simplicity
4. Memory efficiency
5. Strong consistency (in a single node).
6. High availability
7. Durability

## Retrospective
I will not argue with the tremendous popularity of the Redis memory store. It seems that Salvatore's
decision to go for simplicity and deliver features quickly - paid off: today Memcached is a niche
system, and the vast majority of software stacks use Redis. However, I do claim that it is possible to
improve **vastly**  reliability, performance and cost-efficiency metrics of Redis-like memory store.

In my next posts, I am going to go over Redis design principles described above and show how a different
architecture, if aligned better with modern hardware systems, could provide, what I think
a disruptive change to the in-memory datastore market.