+++
title = "balanced vs unbalanced"
tags = ["redis", "dragonfly"]
categories = []
description = ""
menu = ""
banner = "banners/floaty-jerry.png"
images = []
date = 2022-10-30T09:31:00+03:00
draft = false
+++

Or why [Dragonfly](http://dragonflydb.io) is winning hearts of developers...
<!--more-->

Balance is essential in life. When our focus is limited to improving a single aspect of our life, we weaken the whole system. And if you don't provide stability for your system, it will eventually collapse under its own weight and break (or, in the case of Jerry - float).

Take Redis, for example. It supports a relatively high throughput, but its persistency mechanism is not designed to support high loads: the higher the write throughput in Redis, the more fragile the system becomes. To understand why, I need to explain how Redis BGSAVE snapshotting works.

The BGSAVE algorithm allows Redis to produce a point-in-time snapshot of its in-memory data to the disk, or sync with its secondary replica, as a background process.
Redis does this by calling Linux "fork()" to open a child process that can access the parent’s memory. From this point onward, both processes have the same contents, and their physical memory usage stays the same, but as soon as one of them writes into memory, it creates a private copy of the page.

{{< figure src="/img/fork-memory.svg">}}

This feature is called Copy-on-Write, and it's part of Linux's memory management system. The moment a memory page is duplicated by one of the processes, Linux must allocate an additional 4Kb of physical memory.

The Redis child process does not alter its memory pages—it only scans the entries and writes them into an RDB snapshot. Meanwhile, the parent Redis process continues handling incoming writes. With each write that touches an existing memory page, it effectively allocates more physical memory.  Suppose our Redis instance uses 40GB of physical memory. After the fork(), the parent process can potentially duplicate each page, requiring an additional 40GB of RAM: 80GB in total.

But if Redis's write throughput is low enough, it might finish the snapshot process before it duplicates all the pages. In this case, it may end up peaking somewhere between 40GB and 80GB. This uncertain behavior during BGSAVE causes huge headaches to the teams that manage Redis deployments: they need to mitigate the risk of OOM with memory over-provisioning.

KeyDB, a Redis fork that was released with a promise of making Redis faster, can sustain higher throughput due to its multi-threaded architecture. But what happens when BGSAVE is triggered in KeyDB? The KeyDB parent process can handle more write traffic, so it alters more memory pages shared with the child process before BGSAVE finishes. As a result, physical memory usage increases, putting the whole system at greater risk.

Is this KeyDB's fault? No, because KeyDB is not more memory hungry, and its BGSAVE algorithm is the same as that of Redis. But, unfortunately, by solving one problem within the unbalanced architecture of Redis, it introduces others.

This narrow approach is not unique to KeyDB. AWS ElastiCache introduced a similar, closed-source feature that [increases the throughput](https://aws.amazon.com/blogs/database/boosting-application-performance-and-reducing-costs-with-amazon-elasticache-for-redis/) of its Redis instances on multi-CPU machines. Based on my tests, it suffers from the same problem: under a high throughput scenario, it either fails to produce a snapshot or reduces the throughput to ridiculous rates of less than 15K qps.

## Dragonfly snapshotting.

Let's take a step back and formalize the problem. The main challenge of performing point-in-time snapshotting is preserving the snapshot isolation property
when records are altered in parallel: if a snapshot starts at time `t` and ends at `t+u`,
it should reflect the state of all entries as they were at time `t`.

Therefore, we need a scheme for handling records that must be modified due to incoming updates, but have not yet been serialized. And this leads us to the question: how do we even recognize which entries in our snapshotting routine have already been serialized and which haven't?

Dragonfly uses *versioning* for that. It maintains a monotonically increasing counter that
increases with every update. Each dictionary entry in the store captures its “last update” version,
which is then used to determine whether the entry should be serialized. When snapshotting starts, it assigns the next version to itself, thus preserving the following variant for all entries in the store: `entry.version < snapshot.version`.

Then it kicks off the main traversal loop that iterates over the dictionary and starts serializing it to disk. This is done asynchronously, as shown conceptually below with the `fiber_yield()` call.

```cpp {linenos=table,linenostart=199}

snapshot.version = next_version++;

for (entry : table) {
   if (entry.version < snapshot.version) {
     entry.version = snapshot.version;
     SendToSerializationSink(entry);
     fiber_yield();
   }
}

```

The loop is not atomic, and Dragonfly can accept incoming writes in parallel. Each of these write requests can modify, delete or add entries to the dictionary. By checking the entry version in the traversal loop, we ensure that no entry is serialized twice, or that the entries that have been modified after the snapshotting started are not serialized.

The loop above is only part of the solution. We also need to handle updates happening in parallel:

```cpp{linenos=table,linenostart=299}

void OnPreUpdate(entry) {
  uint64_t old_version = entry.version;

  // Update the version to the newest one.
  entry.version = next_version++;  // Regular version update

  if (snapshot.active && old_version < snapshot.version) {
    SendToSerializationSink(entry);
  }
}
```

We make sure that entries that have not been serialized yet (have `version` smaller than `snapshot.version`) will be serialized before they are updated.

Both the main traversal loop and the `OnPreUpdate` hook are enough to make sure that we serialize each existing entry exactly once and entries that are added after the snapshot has started won't be serialized at all. Dragonfly does not rely on OS generic memory management.

By avoiding an unbalanced `fork()` call and by pushing entries to the serialization sink during the update, Dragonfly establishes a natural back-pressure mechanism, which is a must have for every reliable system. As a result of all these factors, Dragonfly's memory overhead is constant during the snapshotting no matter how big the dataset size is.

Is it really this simple? Not quite. While the high-level algorithm as described above is accurate, I have not shown how we coordinate snapshotting across multiple threads,
or how we maintain the `uint64_t` “version” per entry without wasting 8 bytes on each key, etc.

While the snapshotting issue may not seem very significant at first glance, it's actually a cornerstone of the balanced foundation that makes Dragonfly a reliable, performant and scalable system. A feature like snapshotting is one of those things that differentiate Dragonfly from other attempts to build a high-performing memory store, and I think the developer community recognizes this.

{{< figure src="/img/starthist-keydb-df.png">}}

