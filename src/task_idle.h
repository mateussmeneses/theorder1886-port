#ifndef THEORDER_TASK_IDLE_H
#define THEORDER_TASK_IDLE_H
/* Native replacement of the task scheduler's idle callback (eboot 01.02 sub_8a7380).
 *
 * The engine's job workers (sub_20ad20, threads "Task Kernel") poll the scheduler queues
 * (sub_20a950) without pause. After each empty poll they add 1 to a stack counter, which they
 * zero when a job runs, and call the idle callback the scheduler constructor (sub_20b860) got
 * from 0x8a1c78 with a pointer to it. The callback (System V ABI) yields with scePthreadYield
 * and zeroes the counter once it is above 0x708, unless the calling thread is the one stored
 * at eboot 0x1682de8. With nothing to do the workers therefore spin at 100% of a core each.
 *
 * THEORDER_TASK_IDLE selects the callback: 0 keeps the guest code; 1 runs the same policy
 * natively; 2 (default) adds backoff: after THEORDER_TASK_IDLE_SPINS (8) consecutive idle yields
 * (no job ran in between) each further idle period sleeps THEORDER_TASK_IDLE_SLEEP_US (200 us).
 * The streak lives in the counter's high 32 bits, so a job run (counter = 0) ends it exactly.
 * See docs/REA_NATIVE_ABSTRACTION.md. */
#include <stdint.h>
#include <stddef.h>

enum { TASK_IDLE_THRESHOLD = 0x709, TASK_IDLE_STREAK_SHIFT = 32, TASK_IDLE_STREAK_MAX = 0xffff };
typedef enum { TASK_IDLE_NONE, TASK_IDLE_YIELD, TASK_IDLE_SLEEP } TaskIdleAction;
typedef struct { int mode; uint32_t spin_yields; uint64_t sleep_ns; } TaskIdleConfig;

/* What the callback does for counter; *next is the value it stores afterwards (only for an
 * action other than NONE, and only when the calling thread is not the excluded one). */
TaskIdleAction task_idle_policy(uint64_t counter, const TaskIdleConfig *config, uint64_t *next);

/* The patched function: offset and length in the guest image, FNV-1a 64 of its original bytes,
 * and the offset of the 64-bit thread handle it never yields for. */
typedef struct { uint64_t offset, length, fnv1a, excluded; } TaskIdleSite;
extern const TaskIdleSite task_idle_eboot_0102;
typedef struct {
    void *(*self)(void);           /* scePthreadSelf's handle of the calling thread */
    void (*yield)(void);           /* scePthreadYield */
    void (*sleep_ns)(uint64_t ns); /* a host sleep */
} TaskIdleHooks;

uint64_t task_idle_fnv1a(const unsigned char *data, size_t size);
/* Reads THEORDER_TASK_IDLE* into a configuration (mode 2 when unset or invalid). */
TaskIdleConfig task_idle_config_from_env(void);
/* Redirects site->offset of image to the native callback through a 13-byte stub written at stub
 * (within rel32 of the site). Returns 0 when installed, else a reason, leaving image unchanged. */
const char *task_idle_install(unsigned char *image, uint64_t image_size, const TaskIdleSite *site,
                              unsigned char *stub, size_t stub_size, const TaskIdleConfig *config,
                              const TaskIdleHooks *hooks);
/* The callback as the guest calls it (exposed for tests). */
void __attribute__((sysv_abi)) task_idle_callback(uint64_t *counter);
/* Counted per action only: the callback runs about 1800 times per yield. */
typedef struct { uint64_t yields, sleeps; } TaskIdleStats;
TaskIdleStats task_idle_stats(void);
#endif
