/* Exercise the production throttle and MMIO accessors with a controlled clock.
 * Windows additionally uses real competing threads and critical sections. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION TestMutex;
typedef CONDITION_VARIABLE TestCond;
#else
typedef int TestMutex;
typedef int TestCond;
typedef int32_t LONG;
#define InterlockedIncrement(p) __sync_add_and_fetch((p), 1)
#define InterlockedDecrement(p) __sync_sub_and_fetch((p), 1)
#define InterlockedCompareExchange(p, value, expected) __sync_val_compare_and_swap((p), (expected), (value))
#endif
#include "apu_regs.h"
#include "apu_timing.h"

typedef uint64_t hwaddr;
typedef struct {
    TestMutex lock;
    TestCond cond, lock_handoff_cond;
    volatile LONG lock_waiters;
    volatile LONG irq_service_declined;
    bool lock_handoff_waiting;
    uint32_t regs[0x20000];
    unsigned ep_frame_div, next_frame_time_fraction;
    int64_t next_frame_time_us;
    uint64_t output_rebases;
    int sleep_acc_us;
    bool pause_requested;
} MCPXAPUState;

static unsigned checks, failures, waits, handoffs, pending, serviced, drains, irq_updates;
static bool held, threaded;
static int64_t now_us;
static MCPXAPUState *current;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    fprintf(stderr, "Failed line %u: %s\n", (unsigned)__LINE__, #x); } } while (0)
#define QEMU_CLOCK_REALTIME 1
#define QEMU_CLOCK_VIRTUAL 0
#define qatomic_read(p) (*(p))
#define qatomic_set(p, v) (*(p) = (v))
#define qatomic_and(p, v) (*(p) &= (v))

static void qemu_mutex_lock(TestMutex *lock)
{
#ifdef _WIN32
    EnterCriticalSection(lock);
#else
    (void)lock;
#endif
    if (!threaded) { CHECK(!held); held = true; }
}
static void qemu_mutex_unlock(TestMutex *lock)
{
    if (!threaded) { CHECK(held); held = false; }
#ifdef _WIN32
    LeaveCriticalSection(lock);
#else
    (void)lock;
#endif
}
static void service_pending(void);
static void qemu_cond_signal(TestCond *cond)
{
#ifdef _WIN32
    WakeConditionVariable(cond);
#else
    (void)cond;
#endif
}
static void qemu_cond_timedwait(TestCond *cond, TestMutex *lock, int ms)
{
    if (cond == &current->lock_handoff_cond) {
#ifdef _WIN32
        if (threaded) {
            SleepConditionVariableCS(cond, lock, (DWORD)ms);
            return;
        }
#endif
        CHECK(ms > 0 && ms <= 1);
        ++handoffs;
        qemu_mutex_unlock(lock);
        service_pending();
        qemu_mutex_lock(lock);
        return;
    }
    CHECK(!threaded); /* Overdue concurrency case cannot rely on pacing. */
    CHECK(ms > 0);
    ++waits;
    qemu_mutex_unlock(lock);
    service_pending();
    now_us += (int64_t)ms * 1000;
    qemu_mutex_lock(lock);
}
static void qemu_cond_broadcast(TestCond *cond) { (void)cond; }
static int64_t qemu_clock_get_us(int type) { (void)type; return now_us; }
static int64_t qemu_clock_get_ns(int type) { (void)type; return now_us * 1000; }
static void update_irq(MCPXAPUState *d) { (void)d; ++irq_updates; }
#define address_space_memory NULL
static void stl_le_phys(void *space, uint32_t addr, uint32_t val)
{ (void)space; (void)addr; (void)val; }
#include "apu_lock.h"
/* apu_vp.c: guest threads waiting to write a PIO method, and the queue of
 * methods they left for the next lock holder. */
volatile LONG g_apu_pio_waiters;
void mcpx_apu_pio_drain(MCPXAPUState *d);
void mcpx_apu_pio_drain(MCPXAPUState *d)
{
    (void)d;
    if (!threaded) CHECK(held);
    ++drains;
}
#include "throttle.inc"

static void service_pending(void)
{
    unsigned action = pending;
    pending = 0;
    if (!action) return;
    CHECK(!held);
    /* Replace the announced simulated waiter with the real accessor call. */
    InterlockedDecrement(&current->lock_waiters);
    mcpx_apu_write(current, NV_PAPU_IEN, 0x1234, 4);
    CHECK(mcpx_apu_read(current, NV_PAPU_IEN, 4) == 0x1234);
    if (action == 2) current->pause_requested = true;
    if (action == 3)
        mcpx_apu_write(current, NV_PAPU_FECTL, NV_PAPU_FECTL_FEMETHMODE_HALTED, 4);
    ++serviced;
}

static void prepare(MCPXAPUState *d)
{
    memset(d, 0, sizeof(*d));
#ifdef _WIN32
    InitializeCriticalSection(&d->lock);
    InitializeConditionVariable(&d->cond);
    InitializeConditionVariable(&d->lock_handoff_cond);
#endif
    current = d;
    pending = serviced = waits = handoffs = drains = irq_updates = 0;
    now_us = 1000000;
    d->next_frame_time_us = now_us;
    qemu_mutex_lock(&d->lock);
}
static void finish(MCPXAPUState *d)
{
    qemu_mutex_unlock(&d->lock);
#ifdef _WIN32
    DeleteCriticalSection(&d->lock);
#endif
}

static void overdue_epochs(MCPXAPUState *d)
{
    prepare(d);
    for (unsigned frame = 0; frame < 256; ++frame) {
        d->ep_frame_div = frame;
        now_us += 1000; /* 32 samples take longer than their 667us budget. */
        if (!(frame % 8)) { pending = 1; d->lock_waiters = 1; }
        throttle(d);
        CHECK(held);
        CHECK(serviced == frame / 8 + 1);
    }
    CHECK(waits == 0);
    CHECK(handoffs == 32 && serviced == 32);
    CHECK(d->lock_waiters == 0 && !d->lock_handoff_waiting);
    CHECK(d->output_rebases > 0);
    finish(d);
}

static void normal_pacing(MCPXAPUState *d)
{
    prepare(d);
    d->next_frame_time_us += 5500;
    pending = 1;
    d->lock_waiters = 1;
    throttle(d);
    CHECK(serviced == 1 && waits == 1);
    CHECK(now_us == 1005000);
    CHECK(d->next_frame_time_us == 1010833);
    CHECK(d->next_frame_time_fraction == 1);
    CHECK(d->output_rebases == 0);
    for (unsigned frame = 1; frame < 8; ++frame) {
        d->ep_frame_div = frame;
        throttle(d);
    }
    CHECK(waits == 1 && handoffs == 1);
    CHECK(d->next_frame_time_us == 1010833);
    finish(d);
}

static void uncontended_epoch(MCPXAPUState *d)
{
    prepare(d);
    throttle(d);
    CHECK(handoffs == 0 && waits == 0);
    CHECK(now_us == 1000000 && d->next_frame_time_us == 1005333);
    CHECK(d->lock_waiters == 0 && !d->lock_handoff_waiting && held);
    finish(d);
}

static void control_during_handoff(MCPXAPUState *d)
{
    for (unsigned action = 2; action <= 3; ++action) {
        prepare(d);
        pending = action;
        d->lock_waiters = 1;
        throttle(d);
        CHECK(serviced == 1 && held);
        if (action == 2) CHECK(d->pause_requested);
        if (action == 3)
            CHECK(d->regs[NV_PAPU_FECTL] == NV_PAPU_FECTL_FEMETHMODE_HALTED);
        CHECK(waits == 0);
        finish(d);
    }
}

/* The frame thread keeps the device lock across frames: a register read does
 * not wait for it (here, single-threaded, it must not take it at all). */
static void reads_skip_the_lock(MCPXAPUState *d)
{
    prepare(d);
    d->regs[NV_PAPU_IEN] = 0x2468;
    CHECK(mcpx_apu_read(d, NV_PAPU_IEN, 4) == 0x2468);
    CHECK(held && d->lock_waiters == 0);
    finish(d);
}

/* PIO methods queued while the lock was busy run before a later register
 * write from the guest, as they did when each method waited for the lock. */
static void writes_keep_method_order(MCPXAPUState *d)
{
    prepare(d);
    qemu_mutex_unlock(&d->lock);
    mcpx_apu_write(d, NV_PAPU_IEN, 0x1357, 4);
    CHECK(drains == 1 && d->regs[NV_PAPU_IEN] == 0x1357 && !held);
    qemu_mutex_lock(&d->lock);
    finish(d);
}

/* An interrupt delivery the guard declined is served at the next epoch: the
 * line is recomputed (waking the timer thread) and the lock is handed off once,
 * as for a blocked caller, so an overloaded DSP cannot keep the ISR out. */
static void declined_irq_gets_a_handoff(MCPXAPUState *d)
{
    prepare(d);
    d->irq_service_declined = 1;
    throttle(d);
    CHECK(handoffs == 1 && irq_updates == 1 && d->irq_service_declined == 0 && held);
    for (unsigned frame = 1; frame <= 8; ++frame) {
        d->ep_frame_div = frame;
        throttle(d);
    }
    CHECK(handoffs == 1 && irq_updates == 1);
    CHECK(d->lock_waiters == 0 && !d->lock_handoff_waiting);
    finish(d);
}

#ifdef _WIN32
static volatile LONG readers_done, writers_done;
typedef struct { MCPXAPUState *d; HANDLE ready; bool writer; } Client;
static DWORD WINAPI register_client(void *arg)
{
    Client *client = arg;
    SetEvent(client->ready);
    if (client->writer) {
        mcpx_apu_write(client->d, NV_PAPU_IEN, 0x5678, 4);
        InterlockedIncrement(&writers_done);
    } else {
        (void)mcpx_apu_read(client->d, NV_PAPU_IEN, 4);
        InterlockedIncrement(&readers_done);
    }
    return 0;
}
static void competing_register_clients(MCPXAPUState *d)
{
    /* No PIO methods, audio backend or pacing waits can bail out this
     * producer. Both ordinary MMIO clients must finish before it
     * stops producing, including with the optional PIO handoff disabled. */
    threaded = true;
    prepare(d);
    Client clients[2] = {{d, CreateEvent(NULL, TRUE, FALSE, NULL), false},
                         {d, CreateEvent(NULL, TRUE, FALSE, NULL), true}};
    HANDLE threads[2];
    for (unsigned i = 0; i < 2; ++i) {
        threads[i] = CreateThread(NULL, 0, register_client, &clients[i], 0, NULL);
        if (!clients[i].ready || !threads[i]) exit(2);
        CHECK(WaitForSingleObject(clients[i].ready, 2000) == WAIT_OBJECT_0);
    }
    for (unsigned frame = 0; frame < 128; ++frame) {
        d->ep_frame_div = frame;
        now_us += 1000;
        throttle(d);
        if (InterlockedCompareExchange(&readers_done, 0, 0) &&
            InterlockedCompareExchange(&writers_done, 0, 0)) break;
        Sleep(1); /* Model a slow DSP while still owning the device lock. */
    }
    CHECK(InterlockedCompareExchange(&readers_done, 0, 0) == 1);
    CHECK(InterlockedCompareExchange(&writers_done, 0, 0) == 1);
    CHECK(d->regs[NV_PAPU_IEN] == 0x5678);
    CHECK(waits == 0);
    CHECK(d->lock_waiters == 0 && !d->lock_handoff_waiting);
    qemu_mutex_unlock(&d->lock);
    if (WaitForMultipleObjects(2, threads, TRUE, 2000) != WAIT_OBJECT_0) exit(2);
    for (unsigned i = 0; i < 2; ++i) {
        CloseHandle(threads[i]);
        CloseHandle(clients[i].ready);
    }
    DeleteCriticalSection(&d->lock);
    threaded = false;
}

static void recursive_register_access(MCPXAPUState *d)
{
    threaded = true;
    prepare(d);
    /* IRQ service owns the recursive lock while guest MMIO re-enters it. */
    mcpx_apu_lock(d);
    mcpx_apu_write(d, NV_PAPU_IEN, 0x4321, 4);
    CHECK(mcpx_apu_read(d, NV_PAPU_IEN, 4) == 0x4321);
    CHECK(d->lock_waiters == 0 && !d->lock_handoff_waiting);
    qemu_mutex_unlock(&d->lock);
    finish(d);
    threaded = false;
}
#endif

int main(void)
{
    MCPXAPUState *d = calloc(1, sizeof(*d));
    if (!d) return 2;
    overdue_epochs(d);
    normal_pacing(d);
    uncontended_epoch(d);
    control_during_handoff(d);
    reads_skip_the_lock(d);
    writes_keep_method_order(d);
    declined_irq_gets_a_handoff(d);
#ifdef _WIN32
    competing_register_clients(d);
    recursive_register_access(d);
#endif
    free(d);
    printf("APU throttle: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
