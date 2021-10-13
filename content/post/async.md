+++
title = "IO_URING powered event-loop with efficient message passing"
tags = ["io_uring", "fibers", "c++"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2020-09-05T20:30:11+03:00
draft = true
+++

IO_URING is the new API that provides an efficient communication channel with the kernel via shared ring-buffer. It has been specially designed for handling all types of Linux I/O requests and it finally unifies file and networking calls into the same API. This article covers an experimental layer in GAIA that allows building an efficient backend server using IO_URING api. This layer   shows an order of magnitude better throughput on a single server compared to epoll-based approaches. Also, it demonstrates a flexible inter-thread message passing mechanism specially designed for reactive systems.
<!--more-->

[IO_URING](https://kernel.dk/io_uring.pdf) is going to revolutinize how linux-based reactive systems are built. It drastically reduces the number of system calls needed to maintain an I/O communication. In addition, it provides an asynchronous API for file operations, call batching, operations chaining and timeout control. I am not going to explain in depth about the API and instead suggest reading the [IO_URING paper]((https://kernel.dk/io_uring.pdf)).

There are only few examples in the internet that demonstrate how IO_URING can be used, let alone used with Boost.Fibers. In order to test the interactions between IO_URING and Boost.Fibers I've built an experimental layer that allows running share-nothing systems on multi-core systems. The architechture that I adopted uses the following principles:

1. IO_URING powered event-loop per each core. These even-loops are designed to act independently from each other and handle most of their interactions locally inside their threads. The loop is represented by `Proactor` class.

2. `ProactorPool` class that owns and manages all Proactors running in the system. It provides message passing interfaces to run a custom code on any of the Proactor threads.

3. Each server connection is handled by a dedicated fiber. By default all fibers are pinned to their threads and all server connections are randomly distributed between Proactor-threads.

4. There are no locks or mutexes that manage inter-thread communications. At least not on the critical path.

In this post we will cover the basics of I/O event loop powered by IO_Uring and dive in into how we can build an efficient  message passing mechanism. Our goal is to build a reactive thread manager that will support the following scenario:

```
thread_local int val = 0;
Proactor p;
....
p.AsyncRun([] { val = 1});
```

And at some point in time, `val` will be equal to `1` in the proactor thread and `0` in the calling thread. Please note that the whole operation will be done without mutexes or other locks and will
require just few dozens of cpu cycles in both threads. The latency of this asynchronous operation can be as low as few micros - subject to the destination thread availability.

## EventLoop
Below you may find a typical implementation of EventLoop using IO_URING interface. IO_URING serves very similar function to `epoll`, i.e. it allows to harvest incoming event notifications that it has been configured to monitor. Unlike `epoll` interface, however, it provides an additional functionality via `io_uring_submit` call. This call flushes all the outgoing I/O requests from the userland (ring) buffer into kernel. This buffer allows issuing and batching multiple I/O requests without system calls. So, instead of calling `write(fd...)` or `send(socket_fd...)` we fill equivalent structures that are IO_URING specific and write them into a dedicated buffer. This part is out of context of this post, therefore  we assume that other parts of our application fill this buffer but this loop makes sure that the IO_URING records are correctly passed to the kernel threads.

{{<highlight cpp "linenos=table">}}
  Task task;
  while(true) {
     io_uring_submit(&ring, &pending_requests);

     bool noop_loop = true;
     // Process message passing tasks
     while (task_queue_.try_pop(&task)) {
       task.Run();
       noop_loop = false;
     }

     // like epoll_wait(timeout=0).
     io_uring_peek_batch_cqe(&ring, &completions);
     if (completions.empty()) {
       switch_to_other_fibers_if_needed();
     } else {
       noop_loop = false;
       dispatch_completions();
     }


     if (noop_loop) {
      io_uring_wait_cqe(&ring);   // epoll_wait(timeout=-1).
     }
  };
{{</highlight>}}

The part that is in the focus of this post is

```
while (task_queue_.try_pop(&task)) {  // Process message passing tasks
  task.Run();
  noop_loop = false;
}
```

that allows mixing CPU tasks with I/O management. Our CPU may come from the other threads and are queued using the lock-free MPSC queue. By modeling our messages as generic functions we achieve  maximum flexibility in our framework and we do not need to know in advance what type of messages are passed between threads. Consequently, our `AsyncRun` might look like

```
template<typename Func> bool Proactor::AsyncRun(Func&& func) {
  return que_.try_push(std::move(func));  // may fail on overflow.
}

```

The problem with this approach, well, besides the obvious queue overflow problem, is that our event-loop may block on I/O inside `io_uring_wait_cqe` call after the loop processed all the CPU tasks and no new I/O has arrived. In that case it just waits for event notifications similarly to `epoll_wait(timeout=-1)`. If a mesage is sent from another thread it might deadlock since the I/O event may never come and the loop will never process the new tasks.

We could remove the blocking call, of course, and use `io_uring_peek_batch_cqe` that just reaps whatever I/O events are ready but then we would have to poll constantly. Constant polling burns our CPU, messes with metrics, affects our ability to profile and optimize our server. Is there a more intelligent solution to that?

Luckily, we can use [eventfd](https://www.man7.org/linux/man-pages/man2/eventfd.2.html) to signal IO_URING/Epoll loops and wake the `io_uring_wait_cqe()` call. Here, we can register `event_fd` descriptor with IO_URING via `io_uring_prep_poll_add(sqe, event_fd, POLLIN);` we may wake our loop by writing an arbitrary `uint64_t` value into it.

For example,

```
template<typename Func> bool Proactor::AsyncRun(Func&& func) {
  bool res = que_.try_push(std::move(func));  // may fail on overflow.
  if (res) {
    uint64_t val = 1;
    write(event_fd_, &val, sizeof(val));
  }
  return res;
}

```

This will solve the issue with deadlocks but now we have another problem - performance.
You can usually make order of hundreds of thousands systems calls per second on a single server.
We want to build a system that fully utilizes its hardware specifically we prefer to have message passing mechanism as lightweight as possible. Can we reduce number of system calls?
One thing to notice is that we write into `event_fd_` unconditionally, event when Proactor thread is not waiting for I/O. We can reduce number of irrelevant system calls using an atomic variable that will synchronize between Proactor and producer threads.

Assuming that `std::atomic_uint32_t seq_num_` is a member variable of Proactor class we can write the following.


```
const uint32_t WAIT_SECTION_STATE = 1UL << 31;

template<typename Func> bool Proactor::AsyncRun(Func&& func) {
  bool res = que_.try_push(std::move(func));  // may fail on overflow.
  if (res) {
    if (seq_num_.fetch_add(1, std::memory_order_relaxed) == WAIT_SECTION_STATE) {
      uint64_t val = 1;
      write(event_fd_, &val, sizeof(val));
    }
  }
  return res;
}

```

And in proactor thread:

{{<highlight cpp "linenos=table">}}
  Task task;
  seq_num_ = 0;
  while(true) {
     io_uring_submit(&ring, &pending_requests);

     bool noop_loop = true;
     uint32_t seq_num = seq_num_.load(std::memory_order_acquire);

     while (task_queue_.try_pop(&task)) {  // Process message passing tasks
       task.Run();
       noop_loop = false;
     }

     io_uring_peek_batch_cqe(&ring, &completions);  // like epoll_wait(timeout=0).
     if (completions.empty()) {
       switch_to_other_fibers_if_needed();
     } else {
       noop_loop = false;
       dispatch_completions();
     }


     if (noop_loop && seq_num_.compare_and_exchange(seq_num, WAIT_SECTION_STATE, std::memory_order_ack_rel)) {
      io_uring_wait_cqe(&ring);   // epoll_wait(timeout=-1).
      seq_num_.store(0, std::memory_order_release);
     }
  };
{{</highlight>}}

