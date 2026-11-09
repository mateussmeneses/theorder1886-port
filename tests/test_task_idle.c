/* Native task idle callback (src/runtime_task_idle.c, src/task_idle.h).
 *
 * usage: task-idle-test [sub_8a7380.bin]
 * Without arguments: the policy, the install checks and the patched jmp -> stub -> System V
 * callback path on a synthetic function. With the 54 original bytes of eboot 01.02 sub_8a7380
 * (extracted from the user's own game, never committed; docs/REA_NATIVE_ABSTRACTION.md), the
 * original machine code runs in an image with the eboot's layout (its scePthreadSelf and
 * scePthreadYield PLT stubs jump to mocks) and is compared with mode 1 on the same inputs. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/runtime_task_idle.c"
#ifdef _WIN32
#include <windows.h>
static unsigned char *exec_alloc(size_t size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
}
#else
#include <sys/mman.h>
static unsigned char *exec_alloc(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
#endif

static void *self_value;
static unsigned yields_seen, sleeps_seen;
static uint64_t last_sleep;
static void *mock_self(void) { return self_value; }
static void mock_yield(void) { ++yields_seen; }
static void mock_sleep(uint64_t ns) { ++sleeps_seen; last_sleep = ns; }
static __attribute__((sysv_abi)) void *guest_self(void) { return self_value; }
static __attribute__((sysv_abi)) int32_t guest_yield(void) { ++yields_seen; return 0; }
typedef void (__attribute__((sysv_abi)) *GuestIdle)(uint64_t *counter);

static void test_policy(void) {
    TaskIdleConfig same = {1, 8, 200000}, backoff = {2, 3, 200000};
    uint64_t next;
    assert(task_idle_policy(0, &same, &next) == TASK_IDLE_NONE && next == 0);
    assert(task_idle_policy(0x708, &same, &next) == TASK_IDLE_NONE && next == 0x708);
    assert(task_idle_policy(0x709, &same, &next) == TASK_IDLE_YIELD && next == 0);
    assert(task_idle_policy(UINT64_MAX, &same, &next) == TASK_IDLE_YIELD && next == 0); /* saturated */
    /* Backoff: three idle yields, then sleeps; a job run (counter 0) starts over. */
    uint64_t c = 0x709;
    for (int i = 0; i < 3; ++i) {
        assert(task_idle_policy(c, &backoff, &next) == TASK_IDLE_YIELD);
        assert(next == (uint64_t)(i + 1) << 32);
        assert(task_idle_policy(next + 0x708, &backoff, &next) == TASK_IDLE_NONE); /* still polling */
        c = next + 0x709;
    }
    assert(task_idle_policy(c, &backoff, &next) == TASK_IDLE_SLEEP && next == 4ull << 32);
    assert(task_idle_policy(next + 0x709, &backoff, &next) == TASK_IDLE_SLEEP);
    assert(task_idle_policy(0x709, &backoff, &next) == TASK_IDLE_YIELD && next == 1ull << 32);
    /* The streak saturates; 0 spin yields sleeps at once. */
    uint64_t top = (uint64_t)TASK_IDLE_STREAK_MAX << 32;
    assert(task_idle_policy(top + 0x709, &backoff, &next) == TASK_IDLE_SLEEP && next == top);
    TaskIdleConfig eager = {2, 0, 1000};
    assert(task_idle_policy(0x709, &eager, &next) == TASK_IDLE_SLEEP);
}

static void test_env(void) {
#ifdef _WIN32
    _putenv("THEORDER_TASK_IDLE=2"); _putenv("THEORDER_TASK_IDLE_SPINS=4"); _putenv("THEORDER_TASK_IDLE_SLEEP_US=50");
#else
    setenv("THEORDER_TASK_IDLE", "2", 1); setenv("THEORDER_TASK_IDLE_SPINS", "4", 1); setenv("THEORDER_TASK_IDLE_SLEEP_US", "50", 1);
#endif
    TaskIdleConfig c = task_idle_config_from_env();
    assert(c.mode == 2 && c.spin_yields == 4 && c.sleep_ns == 50000);
#ifdef _WIN32
    _putenv("THEORDER_TASK_IDLE=7"); _putenv("THEORDER_TASK_IDLE_SLEEP_US=x");
#else
    setenv("THEORDER_TASK_IDLE", "7", 1); setenv("THEORDER_TASK_IDLE_SLEEP_US", "x", 1);
#endif
    c = task_idle_config_from_env();
    assert(c.mode == 2 && c.sleep_ns == 200000); /* invalid values fall back to the defaults */
#ifdef _WIN32
    _putenv("THEORDER_TASK_IDLE=0");
#else
    setenv("THEORDER_TASK_IDLE", "0", 1);
#endif
    assert(task_idle_config_from_env().mode == 0); /* the opt-out keeps the guest code */
}

/* A synthetic image: an 8-byte "function" at 0x100, the excluded handle at 0x200. */
static void test_install_synthetic(void) {
    enum { SIZE = 0x1000 };
    unsigned char *image = exec_alloc(SIZE + 64);
    assert(image);
    static const unsigned char body[8] = {0x55, 0x48, 0x89, 0xe5, 0x5d, 0xc3, 0x90, 0x90};
    memcpy(image + 0x100, body, sizeof body);
    TaskIdleSite site = {0x100, sizeof body, task_idle_fnv1a(body, sizeof body), 0x200};
    TaskIdleHooks hooks = {mock_self, mock_yield, mock_sleep};
    TaskIdleConfig off = {0, 8, 1000}, same = {1, 8, 1000}, backoff = {2, 1, 1234};
    unsigned char *stub = image + SIZE;
    assert(!strcmp(task_idle_install(image, SIZE, &site, stub, 64, &off, &hooks), "disabled"));
    TaskIdleSite wrong = site;
    wrong.fnv1a ^= 1;
    assert(task_idle_install(image, SIZE, &wrong, stub, 64, &same, &hooks));
    TaskIdleSite outside = site;
    outside.offset = SIZE - 4;
    assert(task_idle_install(image, SIZE, &outside, stub, 64, &same, &hooks));
    assert(task_idle_install(image, SIZE, &site, stub, 12, &same, &hooks)); /* stub too small */
    assert(!memcmp(image + 0x100, body, sizeof body));                     /* refusals change nothing */

    assert(task_idle_install(image, SIZE, &site, stub, 64, &backoff, &hooks) == NULL);
    assert(image[0x100] == 0xe9 && stub[0] == 0x49 && stub[1] == 0xbb && stub[12] == 0xe3);
    GuestIdle idle = (GuestIdle)(void *)(image + 0x100);
    uint64_t handle = 0x1234, counter = 0x709;
    memcpy(image + 0x200, &handle, 8);
    self_value = (void *)(uintptr_t)0x9999;
    yields_seen = sleeps_seen = 0;
    idle(&counter); /* through jmp -> stub -> task_idle_callback */
    assert(yields_seen == 1 && counter == 1ull << 32);
    counter += 0x709;
    idle(&counter);
    assert(sleeps_seen == 1 && last_sleep == 1234 && counter == 2ull << 32);
    self_value = (void *)(uintptr_t)handle; /* the excluded thread never yields nor resets */
    counter = 0x5000;
    idle(&counter);
    assert(yields_seen == 1 && sleeps_seen == 1 && counter == 0x5000);
    TaskIdleStats s = task_idle_stats();
    assert(s.yields == 1 && s.sleeps == 1);
}

/* The eboot layout around sub_8a7380: its PLT stubs and the excluded handle. */
enum { FN = 0x8a7380, PLT_SELF = 0xef3620, PLT_YIELD = 0xef34b0, EXCLUDED = 0x1682de8, IMAGE = 0x1683000 };
static void plt_stub(unsigned char *at, void *target) {
    uintptr_t t = (uintptr_t)target;
    at[0] = 0x49; at[1] = 0xbb; memcpy(at + 2, &t, 8); at[10] = 0x41; at[11] = 0xff; at[12] = 0xe3;
}
static unsigned char *eboot_like(const unsigned char *bytes) {
    unsigned char *image = exec_alloc(IMAGE + 64);
    assert(image);
    memcpy(image + FN, bytes, 54);
    plt_stub(image + PLT_SELF, (void *)guest_self);
    plt_stub(image + PLT_YIELD, (void *)guest_yield);
    return image;
}

static uint64_t next_random(uint64_t *state) {
    *state ^= *state << 13; *state ^= *state >> 7; *state ^= *state << 17;
    return *state;
}

static int test_differential(const char *path) {
    unsigned char bytes[54];
    FILE *f = fopen(path, "rb");
    if (!f || fread(bytes, 1, sizeof bytes, f) != sizeof bytes) {
        fprintf(stderr, "task-idle-test: cannot read 54 bytes from %s\n", path);
        return 1;
    }
    fclose(f);
    assert(task_idle_fnv1a(bytes, sizeof bytes) == task_idle_eboot_0102.fnv1a);
    unsigned char *original = eboot_like(bytes), *native = eboot_like(bytes);
    TaskIdleHooks hooks = {mock_self, mock_yield, mock_sleep};
    TaskIdleConfig same = {1, 8, 1000};
    assert(task_idle_install(native, IMAGE, &task_idle_eboot_0102, native + IMAGE, 64, &same, &hooks) == NULL);
    GuestIdle run_original = (GuestIdle)(void *)(original + FN), run_native = (GuestIdle)(void *)(native + FN);
    static const uint64_t edges[] = {0, 1, 0x707, 0x708, 0x709, 0x70a, 0xffffffffu, 1ull << 32, UINT64_MAX - 1, UINT64_MAX};
    uint64_t state = 0x9e3779b97f4a7c15ull;
    unsigned cases = 0, yielded = 0, excluded_above = 0;
    for (int round = 0; round < 20000; ++round) {
        uint64_t counter = round < 10 ? edges[round] : next_random(&state) >> (next_random(&state) & 63);
        uint64_t handle = 0x1000 + (uint64_t)(round & 3);
        self_value = (void *)(uintptr_t)(0x1000 + (uint64_t)((round >> 2) & 3)); /* excluded one time in four */
        memcpy(original + EXCLUDED, &handle, 8);
        memcpy(native + EXCLUDED, &handle, 8);
        uint64_t a = counter, b = counter;
        unsigned before = yields_seen;
        run_original(&a);
        unsigned original_yields = yields_seen - before;
        before = yields_seen;
        run_native(&b);
        unsigned native_yields = yields_seen - before;
        if (a != b || original_yields != native_yields) {
            fprintf(stderr, "differs for counter 0x%llx: original 0x%llx/%u, native 0x%llx/%u\n",
                    (unsigned long long)counter, (unsigned long long)a, original_yields, (unsigned long long)b,
                    native_yields);
            return 1;
        }
        ++cases;
        yielded += original_yields != 0;
        excluded_above += counter >= TASK_IDLE_THRESHOLD && (uint64_t)(uintptr_t)self_value == handle;
    }
    assert(yielded > 1000 && excluded_above > 1000); /* both branches of the original ran */
    printf("task-idle-test: original sub_8a7380 and mode 1 agree on %u inputs (%u yields, %u excluded)\n", cases,
           yielded, excluded_above);
    return 0;
}

int main(int argc, char **argv) {
    test_policy();
    test_env();
    test_install_synthetic();
    if (argc > 1) return test_differential(argv[1]);
    puts("task-idle-test: ok (no original bytes given: differential test skipped)");
    return 0;
}
