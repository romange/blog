+++
title = "Redis Analysis - Part 2: Simplicity "
tags = []
categories = []
description = ""
menu = ""
banner = "banners/simple.jpg"
images = []
date = 2022-01-17T17:39:50+02:00
draft = true
+++

Let's talk about the simplicity of Redis.
Redis was originally designed as a simple store, and it seems that its APIs achieved this goal.
However, some of Redis internal design choices favored the simplicity of implementation
over the ease of management.

So the question is - what simplicity means to you as a datastore user?

<!--more-->
Before diving into Redis specifics, a disclosure: some of the problems I mention below
do not show up for small workloads. For those of you, who have managed Redis instances
larger than 12GB, please answer yourself the following questions:

 * How well do you know (eight) Redis cache eviction policies? Are you happy with either
one of them?

 * How confident are you when you need to change Redis settings, especially
those that control its memory consumption? Can you guarantee what its peak memory usage will be?

 * Have you ever needed to debug an unresponsive Redis instance or a its OOM crash? How easy was it?

 * Have you ever observed connection overload events when multiple clients connect to your redis instance?


Fourteen years after Redis inception, we've established that the engineering community demands
a simple, low-latency Redis-like API that compliments relational database.
It does not mean that the community is ready to settle with fragile technology
that is hard to manage. API simplicity does not mean that the foundation should be weak.

I was fortunate to observe Redis and Memcached usage globally, so
my opinion was shaped by looking at the whole range of workloads:
I've seen how "simple" design decisions in Redis
caused sub-optimal but manageable quirks with 4-16GB workloads, made it painful at 64GB scale,
and caused frustration and lack of trust nearly with 100+ GB workloads.

## Redis Caching
I mentioned cache policy already. It's one of more significant settings in Redis when used as a cache (a pretty popular use-case for Redis). In a perfect world, a regular user of Redis would love to have a magical cache that does the following:
- does not grow in memory when there is a significant number of expired items. btw, this requirement holds for non-cache scenarios as well.
- similarly, does not evict non-expired entries if there is signicant number of expired items that
  could be evicted instead.
- maximizes hit-ratio in a robust manner by keeping entries that are most likely to be hit in the future.

What an regular user does not want to know is what LFU or LRU is, why Redis does not really implement
any of these caches and why it can not evict expired items efficiently. In reality, not only he must
know the implementation details of Redis cache algorithms, he also needs to choose between eight options of `maxmemory-policy` option. And none of them are doing great.
![redis configs](/img/choices.jpg)

## Persistence
If I had to pick a single design choice that looked "simple" at the time
yet had a substantial negative impact on the reliability of the whole system and the complexity of other features - it would be the fork-based BGSAVE command. The BGSAVE algorithm allows Redis to produce the point-in-time snapshotting of in-memory data or sync with its secondary replica. Redis does it by forking the serialization routine into a child process. By doing so, this child process gets an immutable, point-in-time snapshot of its parent process memory from Linux for "free". Redis relies on Linux property that does not copy the physical memory during `fork()` but uses lazy Copy-On-Write operation instead.

Using OS to achieve consistent snapshot isolation looks like an elegant choice at first sight. However, there are some serious problems with this approach:

1. **Lack of back-pressure** When the Redis parent process mutates its entries, it in fact duplicates Linux memory pages with CoW. CoW is transparent to the parent process; therefore
redis memory component has a hard time estimating its actual memory usage.
And even if it did, there is no efficient mechanism in Redis to "postpone" or stall the incoming writes - the execution thread must progress with the flow. Under the heavy writes, this will easily
cause OOM crashes. ![bgsave](/img/bgsave.gif)
1. **Unbounded memory overhead** In the worst case, both child and parent processes could double
Redis memory physical usage. Unfortunately,  it does not stop there with replica syncs: the parent must also hold the replication log of mutations after the snapshot, which grows in memory until the sync completes. In addition, the parent dataset can grow
beyond its initial size. These factors can contribute to RSS usage spiking wildly
under different write loads or database sizes. All this makes it very hard to estimate the
maximum memory usage for Redis. {{< figure src="/img/boromir.jpg" alt="boromir knows" width="300px">}}
1. **Bad interactions with other Redis features** Ask Redis maintainers how hard it was to implement TLS support in Redis 6. The seemingly unrelated feature had several problems due to how the TLS session interacted with the `fork()` call. As a result: TLS in Redis has mediocre performance.
2. **Linux large pages** Enabling large pages usually improves database performance; however, with Redis and bgsave, huge pages would create high write amplification - a tiny write would cause 2MB or 1GB CoW. This would quickly cause 100% memory overhead and major latency spikes during writes.

So far, I've mentioned a few stability problems with Redis that are impossible to fix
with the current Redis architecture. There is a long tail of additional problems that hurt Redis reliability, impact its resource usage or obscure its API guarantees. Here are some of them in random order:
 - If the master's replication buffer overflows during replica sync, the replica will retry
   the whole synchronization flow, possibly entering the infinite cycle of never-ending attempts.
 - Due to how Redis handles its out-of-band events, its blocking commands can expire with
 timeouts much larger than specified.
 - Redis may keep (TTL) expired items while still growing its main tables under write traffic. Moreover, it can decide on evicting non-expired records even if it has plenty of expired ones.
 - Global commands like `FLUSHDB` "stop the world" during their execution and may take significant time. This means Redis will completely stall processing of ongoing requests for minutes or longer.
 - A similar problem with unresposiveness exists when running LUA scripts.
 - PUB/SUB is unreliable and prone to data loss when a SUB client disconnects.
   Also, PUB/SUB may be a major memory consumer for setups using this feature extensively.

## Fight against complexity
I think it's time to redesign the system that once challenged traditional databases and brought a different paradigm to the community. And I think it is possible to close the gap between
simplicity of product and the shortcomings of technology that powers it.

During the last month and a half I've been working on a new design of cache and dictionary data
structures. Something that could really change how we perceive an inmemory store.
The work is still very much in-progress, but it already shows some interesting properties.
The cache design is so novel that I think it deserves a blog post of its own, so today I will share
its "basic" characteristics like memory efficiency and speed.


{{< figure src="/img/redis_table.png" width="300px">}}
{{< figure src="/img/df_table2.png" width="300px">}}