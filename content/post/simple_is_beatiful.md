+++
title = "Redis Analysis - Part 2: simplicity "
tags = []
categories = []
description = ""
menu = ""
banner = "banners/simple.jpg"
images = []
date = 2022-01-03T17:39:50+02:00
draft = true
+++

Lets talk about simplicity, or more exactly, simplicity of Redis.
Redis was originally designed to be a simple store.
Moreover, simplicity was one of the guiding principles in Redis manifesto.
15 years and 183 redis.conf settings forward, I think simplicity was an overloaded term for Redis -
his authors favored simplicity of implementation over simplicity of usage.

<!--more-->
I know, I know - this is unpopular opinion - after all, most redis-related articles praise Redis
as simple and reliable, some even claim that Redis is a database. Well, I disagree.
I was lucky enough to observe Redis usage at cloud scale, and
my opinion was shaped by looking at the whole range of workloads: from smallest ones to those that
comprise hundreds of gigabytes per node. I've seen how "simple" design decisions in Redis,
caused sub-optimal but manageable quirks with 4-16GB workloads, could become problematic at 64GB scale,
and become nearly unmanageable with 200+ GB workloads.

Take eviction policy, for example. It's one of more important settings in Redis. It has 8 options,
and none of them work very well. As a result, the responsibility of choosing between 8 heuristics
lays on user, who not only needs to learn, say, how LRU and LFU differ,
but also why Redis implementation is not really LRU nor LFU.

But if had to choose a single design decision in Redis that looked "simple" at the time
but had huge negative impact on reliability of the whole system and complexity of other seemingly unrelated
features - that would be fork-based BGSAVE.

BGSAVE allows Redis to maintain consistency (snapshot isolation) when saving rdb snapshots or performing a full replica sync. Redis does it by forking the serializer into a child process that gets from linux a virtual snapshot of its parent
process memory. Redis relies on linux property that does not really copy the physical memory during fork but
uses lazy Copy-On-Write operation instead. Only when a memory page in parent or child processes is mutated the OS
transparently remaps it into two different physical pages thus maintaining data consistency for both chile and parent processes.

On the first sight, looks like an elegant decision - using OS to achieve snapshot isolation.

However, there are serious problems with this approach:
1. **Lack of back-pressure** Under write traffic, a parent mutates entries,
which causes linux memory pages to duplicate. CoW process is transparent to both processes thus
parent process has a hard time to estimate its real memory usage. And even if it did, there is no
efficient mechanism for Redis to "postpone" or stall incoming write -
the execution thread must progress with the program. Under heavy writes this will easily
cause OOM crashes ![bgsave](/img/bgsave.gif).

1. **Unbounded memory overhead** In the worst case both child and parent processes double memory physical usage. Unfortunately, it does not stop there: the parent also holds
the replication log (in case of replica syncs) of mutations happened after the snapshot,
which it must keep until the snapshot sync is completed. And finally, the parent dataset can also grow beyond the initial
size when the snapshot started. These factors can contibute to RSS usage spiking wildly
under differrent write loads or database sizes. All this makes it very hard to estimate the
maximum memory usage for Redis ![maxmemory](/img/boromir.jpg)
1. **Bad interactions with other OS features** Ask Redis maintainers how hard was to implement TLS support
in Redis 6. Seemingly unrelated feature had lots of problems due to how TLS session negotiation between
master and slave interracted with the `fork()` call. As a result: TLS has mediocre performance in
Redis with replication. Another example - linux huge pages.  Huge pages usually help databases with performance
and memory usage. But with Redis, huge pages would cause CoW of 2MB/1GB pages instead of 4KB regular ones.
This would greatly increase memory usage and would cause latency spikes during page copies.




server settings are just a symptom, after all, every mature system has them,
my problem with Redis simplicity is the  which inevitably added with any mature system, it's also about
Well, if you handle a small workload, then probably it is...**for you**.
You probably won't need to tune most of its 183 settings (at the time of writing). If you use it as cache
for a small workload, you probably do not have to learn how lfu differs from lru and what eviction strategy you
should choose out of 8 available eviction policies in Redis.
But with larger workloads you will have more and more questions and slowly but surely you will learn
the inevitable truth: Redis stays "simple" by pushing out the complexity to decide to the user.
And I am sure the original intention was different but after 8 years of development
and by sticking to the original design the end-goal