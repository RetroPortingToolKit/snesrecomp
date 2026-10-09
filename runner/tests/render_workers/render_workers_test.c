#define _POSIX_C_SOURCE 200809L
#include "render_workers.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <process.h>
typedef HANDLE TestThread;
typedef DWORD ThreadIdentity;
static ThreadIdentity identity(void) { return GetCurrentThreadId(); }
static bool same_thread(ThreadIdentity a, ThreadIdentity b) { return a == b; }
static void pause_thread(void) { Sleep(1); }
static unsigned test_cpus(void) { SYSTEM_INFO s; GetSystemInfo(&s); return s.dwNumberOfProcessors; }
#define TEST_ENTRY unsigned __stdcall
#define TEST_RETURN 0
#else
#include <pthread.h>
#include <time.h>
#include <unistd.h>
typedef pthread_t TestThread;
typedef pthread_t ThreadIdentity;
static ThreadIdentity identity(void) { return pthread_self(); }
static bool same_thread(ThreadIdentity a, ThreadIdentity b) { return pthread_equal(a, b) != 0; }
static void pause_thread(void) { struct timespec t = {0, 1000000}; nanosleep(&t, NULL); }
static unsigned test_cpus(void) { long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (unsigned)n : 1; }
#define TEST_ENTRY void *
#define TEST_RETURN NULL
#endif
#define CHECK(e) do { if (!(e)) { fprintf(stderr, "%d: %s\n", __LINE__, #e); exit(1); } } while (0)

enum { ITEMS = 1031 };
typedef struct Batch {
  SnesRenderWorkers *pool;
  unsigned items[ITEMS], value, participants;
  atomic_uint arrived;
  atomic_bool seen[SNES_RENDER_WORKERS_MAX];
  ThreadIdentity threads[SNES_RENDER_WORKERS_MAX];
  bool rendezvous;
} Batch;

static void fill(void *opaque, size_t begin, size_t end, unsigned slot) {
  Batch *batch = opaque;
  CHECK(slot < batch->participants);
  CHECK(begin < end && end <= ITEMS);
  if (batch->rendezvous) {
    if (!atomic_exchange(&batch->seen[slot], true)) {
      batch->threads[slot] = identity();
      atomic_fetch_add(&batch->arrived, 1);
    } else CHECK(same_thread(batch->threads[slot], identity()));
    while (atomic_load(&batch->arrived) < batch->participants) pause_thread();
  }
  /* Reentrant submission must fail promptly instead of deadlocking. */
  CHECK(!SnesRenderWorkersRun(batch->pool, 1, 1, fill, batch));
  for (size_t i = begin; i < end; ++i) batch->items[i] += batch->value;
}

static void init_batch(Batch *batch, SnesRenderWorkers *pool) {
  memset(batch, 0, sizeof(*batch));
  batch->pool = pool; batch->participants = SnesRenderWorkersCount(pool);
  atomic_init(&batch->arrived, 0);
  for (unsigned i = 0; i < SNES_RENDER_WORKERS_MAX; ++i) atomic_init(&batch->seen[i], false);
}

static void check_items(const Batch *batch, unsigned expected) {
  for (unsigned i = 0; i < ITEMS; ++i) CHECK(batch->items[i] == expected);
}

static atomic_bool slots_busy[SNES_RENDER_WORKERS_MAX];
static void checked_fill(void *opaque, size_t begin, size_t end, unsigned slot) {
  CHECK(!atomic_exchange(&slots_busy[slot], true));
  fill(opaque, begin, end, slot);
  CHECK(atomic_exchange(&slots_busy[slot], false));
}

static TEST_ENTRY producer(void *opaque) {
  Batch *batch = opaque;
  for (unsigned i = 0; i < 150; ++i)
    CHECK(SnesRenderWorkersRun(batch->pool, ITEMS, 7, checked_fill, batch));
  return TEST_RETURN;
}

static TestThread start_producer(Batch *batch) {
  TestThread thread;
#ifdef _WIN32
  thread = (HANDLE)_beginthreadex(NULL, 0, producer, batch, 0, NULL); CHECK(thread != NULL);
#else
  CHECK(pthread_create(&thread, NULL, producer, batch) == 0);
#endif
  return thread;
}
static void join_producer(TestThread thread) {
#ifdef _WIN32
  CHECK(WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0); CloseHandle(thread);
#else
  CHECK(pthread_join(thread, NULL) == 0);
#endif
}

typedef struct Bounds { size_t begin[2], end[2]; atomic_uint calls; } Bounds;
static void huge_range(void *opaque, size_t begin, size_t end, unsigned slot) {
  (void)slot;
  Bounds *bounds = opaque;
  unsigned i = atomic_fetch_add(&bounds->calls, 1); CHECK(i < 2);
  bounds->begin[i] = begin; bounds->end[i] = end;
}

int main(void) {
  CHECK(SnesRenderWorkersCount(NULL) == 1);
  CHECK(SnesRenderWorkersCreate(1) == NULL);
  CHECK(SnesRenderWorkersRun(NULL, 0, 0, NULL, NULL));
  CHECK(!SnesRenderWorkersRun(NULL, 1, 0, NULL, NULL));
  SnesRenderWorkersDestroy(NULL);
  Batch serial; init_batch(&serial, NULL); serial.value = 3;
  CHECK(SnesRenderWorkersRun(NULL, ITEMS, 0, fill, &serial)); check_items(&serial, 3);

  SnesRenderWorkers *pool = SnesRenderWorkersCreate(4);
  CHECK(pool != NULL || test_cpus() == 1);
  Batch batch; init_batch(&batch, pool); batch.value = 1;
  batch.rendezvous = true;
  CHECK(SnesRenderWorkersRun(pool, ITEMS, 7, fill, &batch)); check_items(&batch, 1);
  CHECK(atomic_load(&batch.arrived) == batch.participants);
  CHECK(same_thread(batch.threads[0], identity()));
  for (unsigned i = 0; i < batch.participants; ++i)
    for (unsigned j = i + 1; j < batch.participants; ++j)
      CHECK(!same_thread(batch.threads[i], batch.threads[j]));
  /* Persistent identities, exact completion and disjoint ranges across epochs. */
  for (unsigned repeat = 0; repeat < 500; ++repeat)
    CHECK(SnesRenderWorkersRun(pool, ITEMS, repeat % 37, fill, &batch));
  check_items(&batch, 501);
  batch.rendezvous = false;
  CHECK(SnesRenderWorkersRun(pool, ITEMS, SIZE_MAX, fill, &batch)); check_items(&batch, 502);
  CHECK(SnesRenderWorkersRun(pool, 0, 1, NULL, NULL));

  for (unsigned i = 0; i < SNES_RENDER_WORKERS_MAX; ++i) atomic_init(&slots_busy[i], false);
  Batch a, b; init_batch(&a, pool); init_batch(&b, pool); a.value = 2; b.value = 5;
  TestThread first = start_producer(&a), second = start_producer(&b);
  join_producer(first); join_producer(second);
  check_items(&a, 300); check_items(&b, 750);

  Bounds bounds = {0}; atomic_init(&bounds.calls, 0);
  CHECK(SnesRenderWorkersRun(pool, SIZE_MAX, SIZE_MAX - 3, huge_range, &bounds));
  unsigned calls = atomic_load(&bounds.calls);
  CHECK(calls == (pool ? 2u : 1u));
  if (calls == 2 && bounds.begin[0] != 0) {
    size_t temp = bounds.begin[0]; bounds.begin[0] = bounds.begin[1]; bounds.begin[1] = temp;
    temp = bounds.end[0]; bounds.end[0] = bounds.end[1]; bounds.end[1] = temp;
  }
  CHECK(bounds.begin[0] == 0 && bounds.end[calls - 1] == SIZE_MAX);
  if (calls == 2) CHECK(bounds.end[0] == bounds.begin[1]);
  SnesRenderWorkersDestroy(pool);
  for (unsigned repeat = 0; repeat < 25; ++repeat) {
    pool = SnesRenderWorkersCreate(2); init_batch(&batch, pool); batch.value = 9;
    CHECK(SnesRenderWorkersRun(pool, ITEMS, 8, fill, &batch)); check_items(&batch, 9);
    SnesRenderWorkersDestroy(pool);
  }
  puts("Render workers: serial fallback, exact ranges, epochs, concurrency, reentry rejection and lifecycle passed");
  return 0;
}
