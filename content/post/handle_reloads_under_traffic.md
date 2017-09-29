+++
title = "Reloading data structures under high throughput"
tags = ["c++", "concurrency", "backend"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2017-09-29T15:46:55+03:00
draft = false
+++

Suppose you have a multi-threaded server that serves tens of thousands read queries per second. Those queries use a shared data-structure or index that is mostly immutable during the server run with the exception of periodic index reloads.  How do you implement data reloads in that server while keeping it live and kicking in production?

<!--more-->

Obviously, if your data structure is thread-safe and allows mutations while correctly serving reads
then the problem solved. However, it often happens that either the data structure is not thread-safe or its safety guarantees do now allow seamless reads during relatively slow reloads.

Lets examine possible solutions to this problem:

### Shared mutex
First, we can not use exclusive mutex to lock the index due to possibly huge contention in the read path: each query thread will require read access to our index and with the exclusive mutex it will stall all other threads until it finishes reading from the index.
`shared_mutex` will solve this problem, however the reload operation will still need to lock the index exclusively (see below). During that period the read path will be blocked and we came back to the original problem - having seamless reads during reloads.

```cpp

// Query Thread
std::shared_lock<std::shared_mutex> lock(mutex_);   // will block on Reload!
auto res = index_.Query(arg);

.....................

// Reload thread.
std::unique_lock<std::shared_mutex> lock(mutex_);
index.UpdateFromFile(file_name);

```

### managing a pointer to immutable index.

If we allow eventual consistency and our memory capacity can hold 2 indices at the same time we can solve the problem by adding a level of indirection: instead of changing the data structure, we will swap a pointer to our always immutable index.
The naive solution could looks like this:

```cpp

std::atomic<Index*> index_ptr_;

// Query Thread
Index* index = index_ptr_->load();
auto res = index->Query(arg);   // index can be invalid here.
................

// Reload thread.
Index* new_index = new Index(); // At this point we have 2 indices in RAM
new_index->UpdateFromFile(file_name);

new_index = atomic_exchange(&index_ptr_, new_index);  // Swap the pointers.

delete new_index;   // delete the previous index object.
```

The problem with this solution, of course, is the dangling pointer during the read path.
When the ownership over the pointer is managed by reload thread, we can not know when the read threads finish accessing it and it can be safely deleted. This is a classic example where shared_ptr will come to rescue:

```cpp

shared_ptr<Index> index_ptr_;

// Query Thread
shared_ptr<Index> local_copy = atomic_load(&index_ptr_);
auto res = local_copy->Query(arg);
...................

// Reload thread.
std::shared_ptr<Index> new_index = make_shared<Index>(); // Create another index object.
new_index->UpdateFromFile(file_name);

auto prev = atomic_exchange(&index_ptr_, new_index);  // Swap the pointers.
prev.reset();  // or just goes out of scope. In any case we reduce reference count
               // to the old Index.
```

shared_ptr provides built-in atomicity guarantees when incrementing/decrementing its reference count but not when the contents of shared_ptr are fully replaced with the new object instance.
Therefore we must use special functions `atomic_exchange` and `atomic_load` that provide atomicity guarantees for whole shared_ptr object. In other words we must use the atomic transaction when swapping shared_ptr. `atomic_load` is just for synchronizing the read with this transaction.

Unfortunately the approach above has 2 problems:

1. gcc 4.8 does not support `atomic_exchange` for shared_ptr. Even though the code builds it won't be atomic.
2. In versions where `atomic_exchange` is implemented for shared_ptr it's usually done with a global lock shared by potentially several instances of shared_ptr. In other words `atomic_exchange`has suboptimal performance.

Experimental library of C++ has `std::experimental::atomic_shared_ptr` which is designed to overcome the latter issue but it's not in the standard yet. So for now I suggest to use a shared lock which can protect shared_ptr. It might be better to use a cheap spinlock here because we just need to provide atomicity for writing into few memory cells.

I suggest to use an excellent [Folly's RWSpinLock](https://github.com/facebook/folly/blob/master/folly/RWSpinLock.h). Lately they added [SharedMutex](https://github.com/facebook/folly/blob/master/folly/SharedMutex.h) lock but I've not tried it yet.
So the final version (with RWSpinLock) might look like this:

```cpp
RWSpinLock index_lock_;
shared_ptr<Index> index_ptr_;

// Query Thread
index_lock_.lock_shared();
shared_ptr<Index> local_copy = index_ptr_;
index_lock_.unlock_shared();

auto res = local_copy->Query(arg);
..................

// Reload thread.
std::shared_ptr<Index> new_index = make_shared<Index>(); // Create another index object.
new_index->UpdateFromFile(file_name);

index_lock_.lock();
index_ptr_.swap(new_index);
index_lock_.unlock();

new_index.reset();  // or just goes out of scope. In any case we reduce reference count
                    // to the old Index.
```

Please note that it's important to limit locked sections only to pointer changes. Anything else should be outside of lock to reduce the contention. This is why I do not use lock guards in the section above - I want to mark locking explicitly in most straighforward manner. I've seen lots of cases where lock guards were used and it caused huge contention after very innocent code changes were applied later.

It is guaranteed that the read thread protects the lifespan of the index via the local_copy object it holds and it's enough to use shared lock when creating `local_copy` because we do not change the internal contents of `index_ptr_` - just increase the reference count which is atomic.


### Quiescent State-Based Reclamation
Jeff Preshing in his [blog post](http://preshing.com/20160726/using-quiescent-states-to-reclaim-memory/) tackles similar problem but he solves resource deletion issue by using GC method called *quiescent state-based reclamation* (QSBR). Yeah, I needed to look up word *quiescent* in the dictionary as well. It means "in state of inactivity or dormancy". Basically QSBR postpones resource deletion to a time when all the possible threads finished their tasks. Then nothing references the stale resource and it's safe to delete it.

QSBR requires thread cooperation at framework level, i.e. each thread needs to register itself with QSBR manager, and notify it every time it runs a task or finishes it. This way QSBR manager can know when the process has quiescent states and no user-level tasks are running. During this state it runs all the piled deletion callbacks from the last period. The sketch code with this solution will look similar to our first naive version:


```cpp

std::atomic<Index*> index_ptr_;

// Query Thread
// Called by framework:
QSBR()->StartTask();
///

// This load can return either new instance or the old one. Both are fine.
Index* index = index_ptr_->load();
auto res = index->Query(arg);
...
// Called by framework:
QSBR()->EndTask();   // marks end of user-level task for this thread.
.....

///////////////////////////////////////
// Reload thread.
Index* new_index = new Index(); // At this point we have 2 indices in RAM
new_index->UpdateFromFile(file_name);

new_index = atomic_exchange(&index_ptr_, new_index);  // Swap the pointers.

// Schedules the previous instance for deletion.
// This is the main change. We do not delete the index now but ask to run it
// when it's safe.
QSBR()->Schedule([new_index] { delete new_index;});
```

The advantage of this approach is that we do not need to worry about ownership issues. Once we build the solution at the framework level it will always help us with freewing resources asynchronously in a safe manner. In Jeff's example his QSBR algorithm breaks timeline into discrete intervals. All the threads that run tasks during specific interval register themselves with it before running user-level tasks and unregister afterwards. Once all the threads has been unregistered, that interval can be `cleaned` from all the pending callbacks. Another pro


I do not know any other methods for providing responsive reads during data reloads. If you know any - please write in comments.
