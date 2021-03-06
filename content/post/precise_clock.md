+++
title = "Implementing cheap and precise clock"
tags = ["c++", "linux", "posix"]
categories = []
description = ""
menu = ""
banner = ""
images = []
date = 2017-11-02T13:37:51+02:00
draft = false
+++

The posix API for querying high-precision hardware clocks is `clock_gettime`. If one second precision is fine then `time(nullptr)` is your friend. Unfortunately, using precice clocks takes its price - they are more expensive CPU-wise.

<!--more-->
How expensive? On my machine precise versions like `CLOCK_MONOTONIC` and `CLOCK_REALTIME` are 9 times more expensive than `time(nullptr)` and 4 times more expensive than their `COARSE` counterparts. `COARSE` clocks are probably enough as long as you do not need precision better than 10ms.

For a high throughput process that requires sub-ms precision and might call `clock_gettime` hundreds of thousands times per second (for measuring the latency of its incoming requests for example), this function can appear high on CPU profiler radar. Below I provide a solution for a very cheap clock with precision of 0.1ms.

The idea is to cache the clock value inside a process variable and to update it with the required precision. The goal is to make update thread as light as possible and very precise.
The simple solution will be (I omit checking error states for brevity):


```cpp
static std::atomic<int64_t> timer_count_micros;

constexpr uint32_t kNumMicrosPerSecond = 1000000;

static void SleepMicros(uint32_t micros) {
  struct timespec sleep_time;
  sleep_time.tv_sec = micros / kNumMicrosPerSecond;
  sleep_time.tv_nsec = (micros % kNumMicrosPerSecond) * 1000;

  // To protect against signals.
  while (nanosleep(&sleep_time, &sleep_time) != 0 && errno == EINTR) {}
}

static void* UpdateTimerCounter(void* arg) {
  timespec ts;

  int64_t sleep_micros = reinterpret_cast<int64_t>(arg);
  while (IsEnabled()) {
    clock_gettime(CLOCK_REALTIME, &ts);
    timer_count_micros.store(ts.tv_sec * kNumMicrosPerSecond + ts.tv_nsec / 1000)

    SleepMicros(sleep_micros);
  }

  return nullptr;
}

pthread_t InitCacheClock(uint32_t precision_micros) {
  pthread_t result;

  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timer_count_micros.store(ts.tv_sec * kNumMicrosPerSecond + ts.tv_nsec / 1000)

  pthread_create(&result, nullptr, UpdateTimerCounter,
                 reinterpret_cast<void*>(precision_micros / 2));
  return result;
}

```

In short, `UpdateMsCounter` thread synchronizes our process timer variable with latest clock
value. Then it just sleeps and frees the CPU for other tasks. The CPU cost will be linear to our desired precision: lower `precision_micros` is - more iterations our thread will perform.

Unfortunately this implementation has low precision because of the `nanosleep` function: it does not guarantee that the thread will be awaken exactly after `sleep_micros`. In fact, it can take few milliseconds or even more until thread scheduler will context switch to our thread. Another minor disadvantage of this approach that we have 2 system calls per iteration which is a bit costly.

### Using timer file descriptors
Timer file descriptors (in `<sys/timerfd.h>` are linux-specific timers that allow measuring time via file descriptor interface. The advantage of this approach that we can have a thread that does not sleep but still blocks on read and unblocks with desired precision.


```cpp

constexpr uint64_t kPrecisionMicros = 50;
static std::atomic_int timer_fd = ATOMIC_VAR_INIT(-1);

static void* UpdateTimerCounter(void* arg) {
  int fd;
  uint64_t missed;

  // I use `timer_fd` as both cancel signal and container for timer descriptor.
  while ((fd = timer_fd.load(std::memory_order_relaxed)) > 0) {

    // `read` blocks according to timer configuration of `fd`.
    int ret = read(fd, &missed, sizeof missed);
    DCHECK_EQ(8, ret); // it
    timer_count_micros.fetch_add(missed * kPrecisionMicros, std::memory_order_release);
  }
  return nullptr;
}

pthread_t InitCacheClock() {
  /* Set up a periodic timer with cycle = kPrecisionMicros */
  timer_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
  struct itimerspec its;

  its.it_value.tv_sec = 0;
  its.it_value.tv_nsec = kPrecisionMicros * 1000;

  // Setup periodic timer of the same interval.
  its.it_interval = its.it_value;

  timerfd_settime(timer_fd, 0 /* its is RELATIVE */, &its, NULL);


  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  timer_count_micros.store(ts.tv_sec * kNumMicrosPerSecond + ts.tv_nsec / 1000)

  pthread_create(&result, nullptr, UpdateTimerCounter, nullptr);
  return result;
}
```

Usually `missed` variable will hold 1 but if a single iteration was delayed than it will contain values larger than 1. Thus the thread loop is self-adjusting to the clock. The thread does not take much CPU (for sane kPrecisionMicros values), uses a single `read` system call and provides a good alternative for sub-millisecond precision measurements. As I said earlier, the only disadvantage I found with this approach is that it's linux specific.
Please tell me what you think about it or if you thought of better ways to measure clocks.

