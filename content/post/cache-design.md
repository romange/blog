+++
title = "Dragonfly Cache Design"
tags = ["dragonfly", "redis"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2022-06-02T09:53:03+03:00
draft = true
+++

I talked in my [previous post](simple_is_beatiful.md) about Redis eviction policies.
In this post, I would like to describe the design behind Dragonfly cache.
<!--more-->

If you have not heard about [Dragonfly](https://github.com/dragonflydb/dragonfly) - please check it out. It uses - what I hope - novel and interesting ideas backed up by the  research from recent years[1](https://doi.org/10.1007/s00778-014-0377-7)
and [2](https://arxiv.org/abs/2003.07302). It's meant to fix many problems that exist with in-memory datastores today. I have been working on Dragonfly for the last 7 months. It has been one of the more interesting, fulfilling, and challenging projects I've ever done and I do not mean to stop working on it any time soon :)

Anyway, back to cache design. Let's start with a short overview of what LRU cache is and what Redis does. We will see what are the shortcomings of LRU cache in general, and of Redis implementation specifically.

## LRU
The LRU (least recently used) cache policy evicts items that were least recently used. Why?
A cache algorithm strives to optimize a single metric - the hit ratio, or the probability that its items will be accessed in the future.

If a cache is full, it needs to vacate items to make room for new additions. It's reasonable to assume that an item accessed a long time ago, won't be used in the future and is the least valuable to keep. The cache frees space for new additions by removing the least valuable items. **Assuming** that *least recently used* items are *least valuable*, this algorithm will behave optimally. The LRU algorithm is very old, probably older than me, and has been used as a disk cache in first computers.

Unfortunately, this algorithm behaves poorly if the assumption above does not hold. Consider, for example, any access pattern with [Long tail distribution](https://en.wikipedia.org/wiki/Long_tail).

{{< figure src="https://upload.wikimedia.org/wikipedia/commons/thumb/8/8a/Long_tail.svg/1920px-Long_tail.svg.png" alt="long tail distribution" width="350px">}}

In this case, lots of newly added "yellow" items with low access frequency could push out rare "green" items responsible for the vast majority of hits. Memory caches usually hold just a few percent of all the possible data. Therefore the LRU policy may drop its contents together with valuable "green" entities due to traffic fluctuations, just because they are least recently used compared to newly added "yellow" items.

{{< figure src="/img/lru.svg">}}

#### LRU efficiency
LRU is considered a simple algorithm that can be implemented efficiently.
Indeed, we just need to maintain all the items in a double-linked list. When an item is accessed, we just move it to the head of the list. To vacate LRU items, we pop from the tail of the list. See the diagram above. All operations are done in O(1), and the memory overhead per item is 2 pointers, i.e. 16 bytes on 64bit architecture.

## LRU in Redis
Redis implements a few eviction policy heuristics. Some of them are described as "approximated LRU". Why aproximated? Because Redis does not maintain an explicit global order among its items. Instead, it stores the last access timestamp in each entry.

Upon eviction attempt, Redis samples K entries and chooses to vacate an item with the oldest timestamp. This way Redis saves 16 bytes per entry needed for ordering items in one global order. This heuristic is very rough approximation to an LRU which by itself have weaknesses, as we saw before. Redis maintainers [have recently discussed](https://github.com/redis/redis/issues/8947) an option to add additional heuristic to Redis by implementing a classic LRU policy with global order but decided against it.

## Dragonfly Cache
Dragonfly implements cache that a) unlike LRU is resistent to fluctuations of recent traffic, b) does not require random sampling or other approximations like in Redis, c) has no memory overhead per item, not pointers and not a timestamp. Afaik, it's a novel approach for cache design that has not been suggested before.

*I want to thank Ben Manes, the author of wonderful [caffeine package](https://github.com/ben-manes/caffeine) for early comments and the guidance on how to use caffeine simulator in order to compare Dragonfly Cache to other caches.*

Dragonfly Cache is based on another famous cache policy from [1994 paper](https://api.semanticscholar.org/CorpusID:6259428) called "2Q: A Low Overhead High Performance Buffer Management Replacement Algorithm".

2Q addresses the issues with LRU by introducing two independent buffers. Instead of considering just recency as a factor, 2Q also considers access frequency for each item. It first admits recent items into so called probationary buffer (see below). This buffer holds only little part of the whole cache space, say less than 10%. All newly added items compete with each other inside this buffer.

{{< figure src="/img/2q-cache.svg">}}

Only if a probationary item was accessed at least once it is upgraded to the protected buffer. By doing so, it evicts the least recently used item from the protected buffer back to probabtionary buffer. I found [this post](https://arpitbhayani.me/blogs/2q-cache) that explains in more detail about 2Q.

2Q improves LRU in the following sense: just because a new item was added to the cache, does not mean it's useful. Instead, only if was fetched from cache at least once, it's considered to be a high-quality item and it's upgraded to the protected buffer.

It has been shown empirically that 2Q cache is more robust and achives a higher hit-rate than its LRU counterpart.

Ok, so a naive solution could be - to divide Dragonfly entries into two buffers and use a fifo ordering for a probationary buffer and a LRU linked-list for the protected buffer.
Obviosly, it would require using additional metadata and waste precious memory.

Instead, Dragonfly leverages the unique design of [Dashtable](https://github.com/dragonflydb/dragonfly/blob/main/doc/dashtable.md) and uses its weak ordering characteristics for its advantage.

This is how Dashtable segment looks like.

