+++
title = "Gaia Mapreduce Tutorial - part2"
tags = ["c++", "mapreduce", "gaia"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2019-11-01T10:30:12+03:00
draft = false
+++

In [part 1 of my tutorial]({{< ref "gaia_mr.md" >}}) I've explained behind the scenes of a typical mapreduce
framework. In this section we will go over GAIA-MR and will show why it's more efficiant than other open-source frameworks.

<!--more-->

If you recall, [in Section "Reduce API"]({{< ref "gaia_mr.md#reduce-api" >}}), I've explained that a classic MR framework will most likely require a shuffle phase to sort intermediate shards. This phase is necessary because the MR framework guarantees to group all the values for the same key together. Unfortunately, the shuffle phase requires additional disk I/O and CPU, which are the most common bottlenecks for an MR run.

GAIA-MR optimize the mapreduce flow by applying the following changes:

1. It relaxes the framework guarantees and sends the whole shard to a reducer instead of sending `(Key, Array<Value>)` tuples. It becomes a responsibility of a programmer to group values according to the specific use-case he needs. In my experience, it most likely suffices to use in-memory hash-table that merges multiple values for the same key. Luckily, with absl/C++14 it's just a few more lines to write. GAIA-MR guarantees shard locality, i.e., the whole shard will still reach the same reducer instance. Also, a programmer fully controls the order in which the shards from multiple sources arrive to a reducer. Which, in turn, could lead to additional optimizations.

This change eliminates the shuffle phase during which the framework is required to sort each shard before sending it to reducers. The shuffle-phase is usually the most time-consuming phase during a MR run.

2. Gaia MR is multi-threaded and is especially tuned towards efficiently using all the available cores on host. Sharing data between threads is easier and faster than sharing data between different nodes: there is no communication penalty, no network bottleneck.

3. Modern C++ libraries provide very efficient data-structures and utilites that can read, parse and process data much faster than the their according counter-parts in Java, thus adding another factor to the speed-up.

I am going to go over GAIA API in detail in following section, covering most common use-cases: grouping data together by the same key.

# Words count example
“Words count” is one of the classic MapReduce problems. The goal is, given a text repository, to extract words and count their frequency. The text can be a large data file coming from web pages extract. In our case, I’ve used [Common Crawl](https://commoncrawl.org/the-data/get-started/) dataset for my “word count” example. Common Crawl project crawls the internet pages and stores them in special formats on s3. We need [WET](https://commoncrawl.org/the-data/get-started/#WET-Format) files that keep text extracts from the pages without all the metadata and html tags. Once you copy some of them locally, please run `warc_parse <wet_files_glob> --dest_dir=... --num_shards=...` to produce a bunch of raw text files that can be easily processed by the MapReduce. "warc_parse" is just a useful preprocessor that enables us to read regular compressed text files instead of working with WET files. At the end of this step, we should have a sampled Unicode text from the internet, and we are ready to proceed with word_count mr.

## GAIA MR for grouping
We can launch [word_count](https://github.com/romange/gaia/blob/master/examples/wordcount/word_count.cc) on a local machine by running `word_count --use_combine=false --dest_dir=... --num_shards=... <input_text_glob*.gz>`. This pipeline is comprised from a single MR step that does the extraction of words from text files and their frequency counting. Essentially the logic is equivalent to `select word, count(*) from <input_text_glob*.gz> group by 1` in SQL.

The code in the `main` function describes the overall flow of the pipeline.

```cpp
  // Mapper phase
  PTable<WordCount> intermediate_table =
      pipeline->ReadText("inp1", inputs).Map<WordSplitter>("word_splitter", db);
  intermediate_table.Write("word_interim", pb::WireFormat::TXT)
      .WithModNSharding(FLAGS_num_shards,
                        [](const WordCount& wc) { return base::Fingerprint(wc.word); })
      .AndCompress(pb::Output::ZSTD, FLAGS_compress_level);

  // GroupBy phase
  PTable<WordCount> word_counts = pipeline->Join<WordGroupBy>(
      "group_by", {intermediate_table.BindWith(&WordGroupBy::OnWordCount)});
  word_counts.Write("wordcounts", pb::WireFormat::TXT)
      .AndCompress(pb::Output::ZSTD, FLAGS_compress_level);
```

The line `pipeline->ReadText("inp1", inputs).Map<WordSplitter>("word_splitter", db);` instructs the pipeline to read text files passed by inputs array and apply on them mapper `WordSplitter`. We call our MR step "word_splitter". Please note, that the mapper takes raw text line and produces a stream of objects of type `WordCount`. This type is the c++ class defined in the example and is not known
to GAIA framework. In order to help GAIA-MR to serialize this class we must provide `RecordTraits<WordCount>` class specialization in `mr3` namespace [which we do right after](https://github.com/romange/gaia/blob/master/examples/wordcount/word_count.cc#L46) the `WordCount` class definition.

`WordSplitter` mapper produces a stream of `WordCount` objects which represented by a handle `intermediate_table` of type `PTable<WordCount>`. Remember, that MR requires resharding/repartitioning operation that involves flushing the intermediate data on the disk. We must explicitly tell GAIA to write the data, as shown at the snippet below.

```cpp
intermediate_table.Write("word_interim", pb::WireFormat::TXT)
      .WithModNSharding(FLAGS_num_shards,
                        [](const WordCount& wc) { return base::Fingerprint(wc.word); })
      .AndCompress(pb::Output::ZSTD, FLAGS_compress_level);
```

Here we instruct to store `WordCount` objects in TXT format and to shard them using the lambda function that hashes the "word".  We also compress the files before storing them on the disk. The framework writes `FLAGS_num_shards` shards using modulo rule.

To group items by word, we join the intermediate table using `WordGroupBy` class.
Shards from the intermediate table are bound with `WordGroupBy::OnWordCount` method, so the framework knows how to pass data into the joiner. `word_counts` handle represents the output table of `WordGroupBy` operation. We write it into "wordcounts" dataset and finish the pipeline steps.
Similar to other frameworks, the operations described above only instruct the framework how to run. The actual run starts with a blocking call `StartLocalRunner(FLAGS_dest_dir)`. Please note that the destination directory can be either a local directory or a GCS path.

```cpp
PTable<WordCount> word_counts = pipeline->Join<WordGroupBy>(
      "group_by", {intermediate_table.BindWith(&WordGroupBy::OnWordCount)});
  word_counts.Write("wordcounts", pb::WireFormat::TXT)
      .AndCompress(pb::Output::ZSTD, FLAGS_compress_level);
LocalRunner* runner = pm.StartLocalRunner(FLAGS_dest_dir);
```


## WordSplitter Mapper
`WordSplitter` class is the mapper class that is responsible for the word extraction and for emitting words further into the pipeline. The example allows multiple options, using different regex engines to extract the data or using `combiner` optimization to reduce data volume passed in the pipeline. We won't cover these options for now. In our case `WordSplitter` uses the basic flow and it calls `cntx->Write(WordCount{string(word), 1});`  to emit `<word,1>` pair. Once it's emitted
it is serialized and added to the appropriate shard according to the sharding function.
By default, our mapper instances use hyperscan regex engine `db` and they must accept it in the constructor. In order to pass constructor arguments to mappers we just pass them into `Map<MyMapperClass>("stepname", args...);` call that makes sure to pass them along when mapper instances are created. This demonstrates that we can pass global resources outside the framework into our classes in a convenient way.

## WordGroupBy joiner
`WordGroupBy` instances are created by the framework and they process each their own shard using
member method `OnWordCount`. The framework will call this method per each record in the shard. It does not guarantee a specific order, but it guarantees that the same shard will reach the same joiner instance fully (shard locality).

For the word count logic we do not need to sort words, just to count them, so we use a hash table `WordCountTable` class to count them. `OnWordCount` does not output any data because at this point the counts for any of the processed words are not finalized yet. Eventually the intermediate shard is processed by joiner instance and then `OnShardFinish` is called. It goes over the hash table and outputs the final `<word, count>` pairs.

As you can see writing GAIA MRs is not complicated as long as you understand the mechanics of Mapreduce systems. As a bonus you get unprecendented performance and the ability to debug, monitor and profile your MR code on your machine.
