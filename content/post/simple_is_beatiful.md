+++
title = "Redis Analysis - Part 2: Simplicity "
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
Redis was originally designed to be a simple store. However, some of its design decisions hint
that his authors favored the simplicity of implementation over the simplicity of usage.
So the question is - what simplicity means to you as a database user?

<!--more-->
Let me rephrase the question. Time for the Redis Quiz!

 * Do you appreciate that your favorite memory store uses a simple hash-table
implementation underneath or you would rather not know the secret sauce underneath but get
much better memory usage and robust performance?

 * Do you know all eight Redis cache eviction policies by heart? Are you happy with either
one of them?

 * How confident are you when you need to change any of 180 redis settings, especially
those that control its memory consumption?

I think that 14 years after its inception, Redis is not simple to operate, and ironically,
its codebase is quite complicated as well. The latter btw, was partially caused by walkarounds that
tried to fix Redis core design inefficiencies.

I was lucky enough to observe Redis usage at global scale, and
my opinion has been shaped by looking at the whole range of workloads: from smallest ones to those that
comprise hundreds of gigabytes per node. I've seen how "simple" design decisions in Redis,
caused sub-optimal but manageable quirks with 4-16GB workloads, become painful at 64GB scale,
and become nearly unmanageable with 100+ GB workloads.

I mentioned cache policy already. It's one of more important settings in Redis when it's used
as a cache (quite popular use-case for Redis). This setting has 8 options,
and none of them are great due to how the redis cache was designed.
The responsibility to choose the best option falls on a Redis administrator.
![redis configs](/img/choices.jpg)

But if I had to pick a single design decision in Redis that looked "simple" at the time
but had huge negative impact on reliability of the whole system and complexity of other
features - that would be the fork-based BGSAVE command.

BGSAVE algorithm allows Redis to produce the point-in-time snapshotting of in-memory data or
perform a consistent sync with its secondary replica. Redis does it by forking the serializer into a child process. By doing so, this child process gets
a virtual snapshot of its parent process memory from Linux for "free". Redis relies on Linux property that does not really copy the physical memory during `fork()` but
uses lazy Copy-On-Write operation instead. On the first sight, looks like an elegant decision - using OS to achieve consistent snapshot isolation of it database structures.

However, there are serious problems with this approach:
1. **Lack of back-pressure** When the parent process mutates entries, it causes
Linux to duplicate its memory pages. CoW process is transparent to tje parent process thus
it has a hard time to estimate its real memory usage. And even if it did, there is no
efficient mechanism for Redis to "postpone" or stall the incoming writes -
the execution thread must progress with the program. Under heavy writes this will easily
cause OOM crashes ![bgsave](/img/bgsave.gif).
1. **Unbounded memory overhead** In the worst case, both child and parent processes could double
Redis memory physical usage. Unfortunately, for replica sync cases it does not stop there: the parent also holds
the replication log (in case of replica syncs) of mutations happened after the snapshot,
which it must keep until the snapshot sync is completed. In addition, the parent dataset can grow
beyond its initial size. These factors can contibute to RSS usage spiking wildly
under differrent write loads or database sizes. All this makes it very hard to estimate the
maximum memory usage for Redis ![maxmemory](/img/boromir.jpg)
1. **Bad interactions with other Redis features** Ask Redis maintainers how hard was to implement TLS support
in Redis 6. Seemingly unrelated feature had lots of problems due to how TLS session negotiation between
master and slave interracted with the `fork()` call. As a result: TLS has mediocre performance in
Redis with replication. Another example - linux huge pages.  Huge pages usually boost databases performance.
However with Redis, huge pages would cause CoW of 2MB/1GB pages during `fork()` which would result in
catastrophic memory overhead with latency spikes during page copies.

So far, I mentioned 2-3 stability problems with Redis that are hard (I think impossible) to fix with
current Redis architecture. However there is a long list of smaller problems that hurt Redis reliability
or impact its resource usage. Here some of them in random order:
 - During replica sync, if master's replication buffer is overflowed, the replica will retry
   the whole synchronization flow, possible entering the infinite cycle of never ending attempts.
 - Due to Redis synchronous execution flow, its blocking commands can expire with deadlines
   much larger than requested.
 - Redis might keep (TTL) expired items while still increasing its memory usage.
   Moreover, it can decide on evicting non-expired records even if it has plenty of expired ones.
 - Database-wide commands like `FLUSHDB` stop the world during their execution. Which means they
   stall the regular request processing of redis to minutes or dozen of minutes. That includes `INFO`
   command, which is used to exposes Redis metrics. Bottom line - Redis may become unresponsive to any external
   probe due to a heavy command running. Lately they added `ASYNC` modifier and
   corresponding server setting that allows flushing
   the database asyncronously. Speaking of tradeoffs and complexity - what's not immediately clear
   is that this modifier can impact Redis peak memory usage during heavy writes.
 - A similar problem with unresposiveness exists when running LUA scripts.
 - PUB/SUB is unreliable and prone to data loss in case a SUB client disconnects.
   Also, PUB/SUB may be a major memory consumer for setups using this feature extensively.



