+++
title = "Gaia Mapreduce Tutorial - part2"
tags = ["c++", "mapreduce", "gaia"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2019-08-12T21:30:12+03:00
draft = true
+++

In [part 1 of my tutorial]({{< ref "gaia_mr.md" >}}) I've explained behind the scenes of a typical mapreduce
framework. In this section we will go over GAIA-MR and will show why it's more efficiant than other open-source frameworks.

<!--more-->

If you recall, [in Section "Reduce API"]({{< ref "gaia_mr.md#reduce-api" >}}), I've explained that a classic MR framework will most likely require a shuffle phase to sort intermediate shards. This phase is necessary because the MR framework guarantees to group all the values for the same key together. Unfortunately, the shuffle phase requires additional disk I/O and CPU, which are the most common bottlenecks for an MR run.

GAIA-MR optimize the mapreduce flow by applying the following changes:

1. It relaxes the framework guarantees and sends the whole shard to a reducer instead of sending `(Key, Array<Value>)` tuples. It becomes a responsibility of a programmer to group values according to the specific use-case he needs. In my experience, it suffices to use in-memory hash-table that merges multiple values for the same key for the majority of cases. Luckily, with absl/C++14 it's just a few more lines to write. GAIA-MR guarantees shard locality, i.e., the whole shard will still reach the same reducer instance. Also, a programmer fully controls the order in which the shards from multiple sources arrive to a reducer. Which, in turn, could lead to additional optimizations.

This change eliminates the shuffle phase during which the framework is required to sort each shard before sending it to reducers. The shuffle-phase is usually the most time-consuming phase during a MR run.

2. Gaia MR is multi-threaded and is especially tuned towards efficiently using all the available cores on host. Sharing data between threads is easier and faster than sharing data between different nodes: there is no communication penalty, no network bottleneck.

3. Modern C++ libraries provide very efficient data-structures and utilites that can read, parse and process data much faster than the their according counter-parts in Java, thus adding another factor to the speed-up.

I am going to go over GAIA API in detail in following sections, covering two most common use-cases: grouping data together by the same key and joining multiple sources of data
for the same key.

# GSOD example
I've used [GSOD weather dataset](https://console.cloud.google.com/bigquery?p=bigquery-public-data&d=samples&t=gsod&page=table) from [Google Bigquery Public Datasets](https://cloud.google.com/bigquery/public-data/) for my "group by" example. This data contains historic weather measurements in USA, but for this example, we will use only few fields from each record.

In order to access the dataset, we need to export it to google cloud storage (GCS) first.
I've prepared publicly accessible reduced dataset at `gs://kushkush/gsod/` that you can copy
to you local disk. We will go over [gsod_group.cc](https://github.com/romange/gaia/blob/master/examples/gsod_group.cc) example that demonstrates a very simple MR flow that reads
the input data and counts number of measurements

## GAIA MR for grouping

## GAIA MR for joining
