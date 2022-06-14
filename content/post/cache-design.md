+++
title = "Dragonfly Cache Design"
tags = ["dragonfly", "redis"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2022-06-14T09:53:03+03:00
draft = true
+++

I talked in my [previous post]({{< ref "simple_is_beatiful.md" >}}) about Redis eviction policies.
In this post, I would like to describe the design behind Dragonfly cache.
<!--more-->

If you have not heard about [Dragonfly](https://github.com/dragonflydb/dragonfly) - please check it out. It uses - what I hope - novel and interesting ideas backed up by the  research from recent years [[1]](https://doi.org/10.1007/s00778-014-0377-7)
and [[2]](https://arxiv.org/abs/2003.07302). It's meant to fix many problems that exist with Redis today. I have been working on Dragonfly for the last 7 months and it has been one of the more interesting and challenging projects I've ever done!

Anyway, back to cache design. We'll start with a short overview of what LRU cache is and its shortcomings in general, and then analyze Redis implementation specifically.

## LRU
The least recently used (LRU) cache policy evicts items, as the name implies, that were least recently used. It works this way because a cache algorithm strives to optimize the hit ratio, or the probability that its items will be accessed in the future.


If a cache is full, it needs to vacate items to make room for new additions. The cache frees space for new additions by removing the least valuable items, **assuming** that the *least recently used* item is also the *least valuable*.

The assumption is reasonable, but unfortunately, this algorithm behaves poorly if the assumption above does not hold. Consider, for example, an access pattern with [Long tail distribution](https://en.wikipedia.org/wiki/Long_tail). Here, the y-axis represents the normalized access frequency of the items in cache and the x-axis represents the items ordered from highest frequency to lowest.

{{< figure src="https://upload.wikimedia.org/wikipedia/commons/thumb/8/8a/Long_tail.svg/1920px-Long_tail.svg.png" alt="long tail distribution" width="350px">}}

In this case, lots of newly added "yellow" items with low access frequency could push out rare but valuable "green" items responsible for the vast majority of hits. As a result, the LRU policy may sweep its contents together with valuable "green" entities due to traffic fluctuations.

{{< figure src="/img/lru.svg">}}

### LRU implementation efficiency
LRU is a simple algorithm that can be implemented efficiently.
Indeed, it maintains all the items in a double-linked list. When an item is accessed, LRU moves it to the head of the list. To vacate LRU items, it pops from the tail of the list. See the diagram above. All operations are done in `O(1)`, and the memory overhead per item is 2 pointers, i.e. 16 bytes on 64bit architecture.

### LRU in Redis
Redis implements a few eviction policy heuristics. Some of them are described as "approximated LRU". Why approximated? Because Redis does not maintain an exact global order among its items like in classic LRU. Instead, it stores the last access timestamp in each entry.

When it needs to evict an item, Redis performs random sampling of the entire keyspace and selects K candidates. Then it choosed the item with the least recently used timestamp among those K candidates and vacates it. This way, Redis saves 16 bytes per entry needed for ordering items in one global order. This heuristic is a very rough approximation of an LRU.Redis maintainers [have recently discussed](https://github.com/redis/redis/issues/8947) the possibility of adding an additional heuristic to Redis by implementing a classic LRU policy with global order but eventually decided against it.

## Dragonfly Cache
Dragonfly implements cache that:
  * is resistant to fluctuations of recent traffic, unlike LRU.
  * Does not require random sampling or other approximations like in Redis.
  * Has **zero** memory overhead per item.
  * Has very small `O(1)` run-time overhead.

It's a novel approach for cache design has not been suggested before in academic research.

Dragonfly Cache (dash cache) is based on another famous cache policy from [1994 paper](https://api.semanticscholar.org/CorpusID:6259428) - "2Q: A Low Overhead High Performance Buffer Management Replacement Algorithm".

2Q addresses the issues with LRU by introducing two independent buffers. Instead of considering just recency as a factor, 2Q also considers access frequency for each item. It first admits recent items into a so-called *probationary buffer* (see below). This buffer holds only a little part of the cache space, say less than 10%. All newly added items compete with each other inside this buffer.

{{< figure src="/img/2q-cache.svg">}}

Only if a probationary item was accessed at least once, will it be proven as *worthy*, and upgraded to the protected buffer. By doing so, it evicts the least recently used item from the protected buffer back to probationary buffer. You can read [this post](https://arpitbhayani.me/blogs/2q-cache) for more details.

2Q improves LRU by acknowledging that just because a new item was added to the cache - does not mean it's useful. 2Q requires an to have been accessed at least once before to be considered as a high quality item. In this way, 2Q cache has been proven to be more robust and to achieve a higher hit rate than LRU policy.

### Dashtable in 30 seconds
I suggest to read [the original paper](https://arxiv.org/abs/2003.07302) about Dashtable, or at least to go over my [overview in github](https://github.com/dragonflydb/dragonfly/blob/main/docs/dashtable.md).

What we need to know here is several facts about Dashtable in Dragonfly implementation:

1. It's comprised of segments of constant size. Each segment holds 56 regular buckets with multiple slots. Each slot has space for a single item.
2. Dashtable routing algorithm uses item's hash value to compute its segment id. In addition, it predefines 2 out of 56 buckets where the item can reside within the segment. The item can reside in any free slot in those two buckets.
3. In addition to regular buckets, a Dashtable segment manages 4 stash buckets that may gather overflow items that do not have space in their assigned buckets. The routing algorithm never assigns stash buckets directly. Instead, only if its two home buckets are full, the item is allowed to reside in any of 4 stash buckets. This greatly increases segment's utilization.
4. Once segment becomes full and there is no free place for a new item in its home buckets nor in stash buckets, the Dashtable grows by adding a new segment and splitting the contents of the full segment roughly in half.

{{< figure src="/img/segment.svg">}}

So, this point in time, when segment is full is a convenient point to add any type of eviction policy to Dashtable because this is when Dashtable grows. Moreover, in order to prevent growth of Dashtable we can only evict items from that full segment. Hence, we got ourselves very precise eviction framework that operates in `O(1)` run-time complexity.

### 2Q implementation

A naive solution could be - to divide Dragonfly entries into two buffers and use a FIFO ordering for a probationary buffer and an LRU linked-list for the protected buffer.
Obviosly, it would require using additional metadata and waste precious memory.

Instead, Dragonfly leverages the unique design of [Dashtable](https://github.com/dragonflydb/dragonfly/blob/main/doc/dashtable.md) and uses its weak ordering characteristics to its advantage.

To implement 2Q we need to explain how we define probationary and protected buffers in Dashtable, how we promote a probationary item into protected buffer and how we evict items from the cache.

{{< figure src="/img/dash-cache.svg">}}

We overlay the following semantics on the original Dashtable:
1. Slots within a bucket now have rank or precedence. A slot on the left has the highest rank - `0`, and the last slot on the right has the lowest rank.
2. Stash buckets within a segment serve as probationary buffer. When a new item is added to the full segment, it's been added into a stash bucket at slot 0. All other items in the bucket are shifted right, and the last item in the bucket is evicted. This way the bucket serves as FIFO queue for probationary items.
3. Every cache hit "promotes" its item: a) if the item was in a stash bucket, it's been moved immediately into its home bucket to the last slot; b) if it was in a home bucket at slot `i`, it's been swapped out with item at slot `i-1`. c) item at slot `0` stays in the same place.
4. When a probationary item is promoted to the protected bucket, it's been moved to the last slot there. The item that was there before is being demoted back into the probationary bucket.

So basically, Dash-Cache eviction policy is comprised of an "eviction" step described by (2) and the positive reinforcement step described by (3).

That's it. No additional metadata is needed. Very quickly high quality items will reside at lower slot indices in their home buckets. Newly added items will compete with each other within stash/probationary buckets. In our implementation each bucket has 14 slots, hence each probationary item can be shifted 14 times before being evicted from the cache, unless it proves its usefulness and be promoted. Each segment has 56 regular buckets and 4 stash buckets, therefore we have `6.7%` space allocated for probationary buffer. It's enough to catch high quality items before they are evicted.

I hope you enjoyed to see how Dragonfly utilizes seemingly unrelated concepts together to its advantage.

*I want to thank [Ben Manes](https://twitter.com/benmanes), the author of wonderful [caffeine package](https://github.com/ben-manes/caffeine) for early comments and the guidance on how to use caffeine simulator in order to compare Dragonfly Cache to other caches.*