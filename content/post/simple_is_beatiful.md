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

Let's talk about the simplicity, or more precisely, the simplicity of Redis.
Redis was originally designed as a simple store, and it seems that its APIs reached this goal. However, some of its design choices favored the simplicity of implementation over ease of use.

So the question is - what simplicity means to you as a datastore user?

<!--more-->
Specifically,

 * Is it important to you as a user that your favorite memory store uses a simple hash-table implementation underneath, or you would rather not know the secret sauce underneath but get much better memory usage and robust performance?

 * Do you know all eight Redis cache eviction policies by heart? Are you happy with either
one of them?

 * How confident are you when you need to change any of redis settings, especially
those that control its memory consumption? Can you guarantee what Redis peak memory usage will be?

I think that 14 years after its inception, Redis is not simple to operate, and ironically,
its codebase is quite complicated as well. The latter btw was partially caused by walkarounds that
tried to fix Redis's core design inefficiencies.

I was lucky enough to observe Redis usage at a global scale, and
my opinion was shaped by looking at the whole range of workloads: from the smallest ones to those that comprise hundreds of gigabytes per node. I've seen how "simple" design decisions in Redis
caused sub-optimal but manageable quirks with 4-16GB workloads, made it painful at 64GB scale, and caused frustration and lack of trust nearly with 100+ GB workloads.

I mentioned cache policy already. It's one of the more major settings in Redis when used as a cache (a quite popular use-case for Redis). This setting has eight options, yet none are doing great due to how the Redis cache is designed. The responsibility to choose the best option falls on a Redis administrator.
![redis configs](/img/choices.jpg)

But if I had to pick a single design choice that looked "simple" at the time
yet had a huge negative impact on the reliability of the whole system and the complexity of other features - it would be the fork-based BGSAVE command. The BGSAVE algorithm allows Redis to produce the point-in-time snapshotting of in-memory data or sync with its secondary replica. Redis does it by forking the serialization routine into a child process. By doing so, this child process gets an immutable, point-in-time snapshot of its parent process memory from Linux for "free". Redis relies on Linux property that does not really copy the physical memory during `fork()` but uses lazy Copy-On-Write operation instead.

Using OS to achieve consistent snapshot isolation looks like an elegant choice at first sight. However, there are some serious problems with this approach:

1. **Lack of back-pressure** When the parent process mutates entries, it causes Linux to duplicate its memory pages. CoW process is transparent to the parent process. Thus
it has a hard time estimating its physical memory usage. And even if it did, there is no
efficient mechanism in Redis to "postpone" or stall the incoming writes -
the execution thread must progress with the program. Under the heavy writes, this will easily
cause OOM crashes. ![bgsave](/img/bgsave.gif)
1. **Unbounded memory overhead** In the worst case, both child and parent processes could double
Redis memory physical usage. Unfortunately,  it does not stop there with replica syncs: the parent must also hold the replication log of mutations that occurred after the snapshot, which grows in memory until the sync completes. In addition, the parent dataset can grow
beyond its initial size. These factors can contibute to RSS usage spiking wildly
under differrent write loads or database sizes. All this makes it very hard to estimate the
maximum memory usage for Redis. {{< figure src="/img/boromir.jpg" alt="boromir knows" width="300px">}}
1. **Bad interactions with other Redis features** Ask Redis maintainers how hard was to implement TLS support in Redis 6. The seemingly unrelated feature had lots of problems due to how TLS session negotiation between master and slave interacted with the `fork()` call. As a result: TLS has mediocre performance in Redis with replication. Another example - Linux large pages. Large pages usually boost databases performance, however, with Redis and bgsave, huge pages would cause CoW of 2MB or 1GB pages during `fork()`. This would result in catastrophic memory overhead with latency spikes during page copies.

So far, I mentioned 2-3 stability problems with Redis that are hard (I think impossible) to fix with current Redis architecture. However there is a long list of smaller problems that hurt Redis reliability, impact its resource usage or obscure its API guarantees. Here some of them in random order:
 - During replica sync, if master's replication buffer is overflowed, the replica will retry
   the whole synchronization flow, possible entering the infinite cycle of never ending attempts.
 - Due to how Redis handles it's out-of-band events, its blocking commands can expire with
 timeouts much larger than specified.
 - Redis may keep (TTL) expired items while still growing its dictionary tables under write traffic. Moreover, it can decide on evicting non-expired records even if it has plenty of expired ones.
 - Database-wide commands like `FLUSHDB` stop the world during their execution and may take significant time. This means Redis may stall the processing of its other requests for minutes or longer. That includes the `INFO` command that exposes Redis metrics. The bottom line - Redis may become unresponsive to any external probe due to a heavy command running. Lately, they added the `ASYNC` modifier and the corresponding server setting that allows emptying
   a database asynchronously. Speaking of tradeoffs and complexity - it's not immediately clear how it affects memory usage during heavy writes.
 - A similar problem with unresposiveness exists when running LUA scripts.
 - PUB/SUB is unreliable and prone to data loss in case a SUB client disconnects.
   Also, PUB/SUB may be a major memory consumer for setups using this feature extensively.

Quoting Redis Manifesto, "We believe designing systems is a fight against complexity".
I think it's time to redesign the system that once upon a time has challenged the traditional databases market but now has its own gravitational force with its own baggage.

Enough rants, time for a demo! I've been busy during last month, working on a novel data structure
that changes completely how we can use our inmemory stores and their caching efficiency. Sorry, no code yet - it's still early stages, mostly work in progress. However, I would like to share its basic characteristics.

{{< figure src="/img/redis_table.png" width="300px">}}
{{< figure src="/img/df_table.png" width="300px">}}