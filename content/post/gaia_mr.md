+++
title = "Gaia Mapreduce Tutorial - part1"
tags = ["c++", "mapreduce", "gaia"]
categories = []
description = "" # If I put here something then first part is not shown.
menu = ""
banner = ""
images = []
date = 2019-07-11T10:52:35+03:00
draft = true
+++

There are many Java-based mapreduce frameworks that exist today -
Apache [Beam](https://beam.apache.org/), [Flink](https://flink.apache.org/), [Apex](https://apex.apache.org/) are to name few.

GAIA-MR is my attempt to show advantages of a C++ over other languages in this domain.
It's currently implemented for a single machine but even with this restriction I've seen
up-to 3-7 times reduction in cost and running time vs current alternatives.

Please note that the single machine restriction put a hard limit on how much data we can process, nethertheless GAIA-MR shines with small-to-medium size workloads (upto few hundred gigabytes).
This part gives an introduction about mapreduce in general.

<!--more-->
## Background
Before I start explaining about GAIA-MR, I would like to explain what MapReduce is for.
Originally, [it was introduced by Google](https://research.google.com/archive/mapreduce-osdi04.pdf)
but then was quickly picked up in the industry and rebranded externally as Hadoop.

A mapreduce algorithm is a glorified `GroupBy` or `HashJoin` operation from the DB world.
Its purpose is to go over (big) data (how I hate this term!) and transform it in such a way that pieces of information dispersed across different sources could be grouped for further processing.

One of the big advantages of Mapreduce framework is that it allows a clean separation between low-level algorithms like multi-threading, multi-machine communication, disk-based algorithms, external data structures, efficient I/O optimization on one side, and user-provided pipeline logic on the other side. Another advantage is virtualization of resources that allows the framework to run over multiple CPUs, multiple disks and machines in a way that is semi-transparent to a user.

In other words, a user may focus on his high-level processing task.
All this of course as long as his algorithm can be modeled with one or more MapReduce steps.

#### Mapreduce Step
A mapreduce step is essentially combined from 2 operators: a mapper and a reducer.
Mappers go over 1 or more different input sources comprised of mutually independent records.
A mapper reads each record, applies user-supplied data transformation, possibly filters out or
extends records with more data and then partitions its output according to some rule.
Mappers can multiply the amount of incoming data by 100 or reduce it to almost nothing.
The main restriction they have is that they do not control which records exactly they are gonna process
and they should not assume how many mapper processors will run. They run independently from each other
and usually do not pass information between them.

See ![Map Reduce Step](/img/MapReduce-Tutorial-1.png)

Reducers, on the other hand, read already partitioned data. The data is essentially grouped together according to user-supplied criteria into multiple groups, or as we call them "shards".
Usually those "shards" can be loaded fully into RAM.

Suppose, for example, WalMart wants to join last 10 years of its buyer transactions with another dataset of buyer details. Both sources should be joined by a user id but unfortunately both datasets can not be loaded into RAM due to their size. Mapreduce to the rescue!
We can provide a mapper that just output records from each dataset into
intermediate shards using `user_id % N` rule. Thus we produce `N` shards for each dataset. The mapreduce will colocate information about each user in a single shard per source
at the end of the mapping phase using the sharding function `user_id % N`. N must be large enough to allow loading of each shard into workers memory. The step of moving randomly distributed user id records from the input dataset to be sharded together enables us to do the next step.

Reducer processors load shards `Transaction'(i)` and `UserData'(i)` into RAM. Those shards produced by mappers and they contain all the records for which `user_id % N = i`. After loading those shards, reducers can join them using hash-join or merge sort algolrithms. Eventually reducers produce N output shards that contain joined information. See the picture above. So sharding allows to reduce the size of the minimal working set of data in order to fit into RAM.

#### Classic mapreduce flow and API
Usually a framework is configured by providing instantiations of mapper or reducer classes.

For example, a mapper class may have a function `void Map(InputType input, Context<OutputType>* context)` that is overriden by a developer. In that function he can implement any logic and
call `context->Write(shard_id, joined_key, my_outp)` any number of times per invocation.

The framework instantiates mapper classes on multiple machines processes, processes input data,
stores intermediate shards, possibly sorts and partitions them.

After all it instantiates all the reducer classes. A reducer class might have function
`void Reduce(KeyType key, Stream<ValueType>& stream, Writer<OutputType>* writer)` that will be called with joined key and stream of values corresponding to that key. In walmart example, the `key` would be user_id and `Stream<ValueType>` will contain all the transactions for that user and his user data.
`Reduce` will be called with those arguments by the framework per key and will be able to output its final result via `Writer`. Reducers and Mappers do not talk with each other and usually behave like independent entities. Each `key` will be sent to exactly one reducer that processes its shard.

