/* Native task-scheduler idle callback (src/task_idle.h, docs/REA_NATIVE_ABSTRACTION.md). */
#include "runtime.h"
#include "task_idle.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* eboot 01.02 (CUSA00035): sub_8a7380, 54 bytes up to its ret; the handle it compares with
 * scePthreadSelf is the 64-bit global at 0x1682de8 (rip-relative lea at 0x8a7397). */
const TaskIdleSite task_idle_eboot_0102 = {0x8a7380, 54, 0x15685b7f227bc923ull, 0x1682de8};

static TaskIdleConfig config;
static TaskIdleHooks hooks;
static const uint64_t *excluded_handle;
static _Atomic uint64_t yields, sleeps;

uint64_t task_idle_fnv1a(const unsigned char *data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) h = (h ^ data[i]) * 1099511628211ull;
    return h;
}

TaskIdleAction task_idle_policy(uint64_t counter, const TaskIdleConfig *c, uint64_t *next) {
    uint64_t polls = counter & 0xffffffffu, streak = counter >> TASK_IDLE_STREAK_SHIFT;
    *next = counter;
    if (c->mode != 2) {
        /* The guest policy: the whole counter against the threshold, zero after the yield. */
        if (counter < TASK_IDLE_THRESHOLD) return TASK_IDLE_NONE;
        *next = 0;
        return TASK_IDLE_YIELD;
    }
    if (polls < TASK_IDLE_THRESHOLD) return TASK_IDLE_NONE;
    uint64_t after = streak < TASK_IDLE_STREAK_MAX ? streak + 1 : streak;
    *next = after << TASK_IDLE_STREAK_SHIFT;
    return streak < c->spin_yields ? TASK_IDLE_YIELD : TASK_IDLE_SLEEP;
}

void __attribute__((sysv_abi)) task_idle_callback(uint64_t *counter) {
    uint64_t next;
    TaskIdleAction action = task_idle_policy(*counter, &config, &next);
    if (action == TASK_IDLE_NONE) return;
    if (excluded_handle && (uint64_t)(uintptr_t)hooks.self() == *excluded_handle) return;
    if (action == TASK_IDLE_YIELD) {
        hooks.yield();
        atomic_fetch_add_explicit(&yields, 1, memory_order_relaxed);
    } else {
        hooks.sleep_ns(config.sleep_ns);
        atomic_fetch_add_explicit(&sleeps, 1, memory_order_relaxed);
    }
    *counter = next;
}

static long env_number(const char *name, long fallback, long low, long high) {
    const char *v = getenv(name);
    if (!v || !*v) return fallback;
    char *end;
    long n = strtol(v, &end, 10);
    return *end || n < low || n > high ? fallback : n;
}

TaskIdleConfig task_idle_config_from_env(void) {
    TaskIdleConfig c = {(int)env_number("THEORDER_TASK_IDLE", 2, 0, 2),
                        (uint32_t)env_number("THEORDER_TASK_IDLE_SPINS", 8, 0, 1000000),
                        (uint64_t)env_number("THEORDER_TASK_IDLE_SLEEP_US", 200, 1, 100000) * 1000};
    return c;
}

const char *task_idle_install(unsigned char *image, uint64_t image_size, const TaskIdleSite *site,
                              unsigned char *stub, size_t stub_size, const TaskIdleConfig *c,
                              const TaskIdleHooks *h) {
    enum { STUB_BYTES = 13, JMP_BYTES = 5 };
    if (c->mode < 1 || c->mode > 2) return "disabled";
    if (!h || !h->self || !h->yield || !h->sleep_ns) return "missing hooks";
    if (site->offset > image_size || site->length > image_size - site->offset || site->length < JMP_BYTES ||
        site->excluded > image_size - 8)
        return "site outside the image";
    if (task_idle_fnv1a(image + site->offset, site->length) != site->fnv1a) return "function bytes differ (other build?)";
    if (stub_size < STUB_BYTES) return "no stub space";
    int64_t rel = (int64_t)((intptr_t)stub - (intptr_t)(image + site->offset + JMP_BYTES));
    if (rel < INT32_MIN || rel > INT32_MAX) return "stub out of rel32 range";
    config = *c;
    hooks = *h;
    excluded_handle = (const uint64_t *)(image + site->excluded);
    uintptr_t target = (uintptr_t)task_idle_callback;
    stub[0] = 0x49; stub[1] = 0xbb; /* mov r11, imm64 (r11 is scratch in the System V ABI) */
    memcpy(stub + 2, &target, 8);
    stub[10] = 0x41; stub[11] = 0xff; stub[12] = 0xe3; /* jmp r11 */
    int32_t rel32 = (int32_t)rel;
    unsigned char *at = image + site->offset;
    at[0] = 0xe9; /* jmp rel32: the rest of the original function is never reached */
    memcpy(at + 1, &rel32, 4);
    return NULL;
}

TaskIdleStats task_idle_stats(void) {
    TaskIdleStats s = {atomic_load_explicit(&yields, memory_order_relaxed),
                       atomic_load_explicit(&sleeps, memory_order_relaxed)};
    return s;
}

void runtime_task_idle_report(void) {
    if (config.mode < 1) return;
    TaskIdleStats s = task_idle_stats();
    printf("Runtime: task idle mode %d (spin yields %u, sleep %llu us): yields=%llu, sleeps=%llu\n", config.mode,
           config.spin_yields, (unsigned long long)(config.sleep_ns / 1000), (unsigned long long)s.yields,
           (unsigned long long)s.sleeps);
}
