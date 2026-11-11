/* PS4 memory manager contracts (src/runtime_memory.c): a flexible mapping split by a partial
 * protection change keeps each half's own physical pages (releasing one half leaves the other's
 * contents and pages alone).
 * Linux: build.sh --test (out/memory-test). */
#include "../src/runtime_memory.c"

/* runtime.c pieces runtime_memory.c uses. */
uintptr_t runtime_lookup(const RuntimeExport *table, size_t count, const char *nid) { (void)table; (void)count; (void)nid; return 0; }
static __thread int32_t errno_slot;
int32_t *runtime_errno(void) { return &errno_slot; }

static int failures;
#define CHECK(cond) do { if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static const Vma *vma_at(uintptr_t address) {
    size_t i = vma_index(address);
    return i < vma_count && vmas[i].start <= address ? &vmas[i] : NULL;
}

/* A flexible mapping split by mprotect: each half keeps its own physical pages. */
static void flexible_split_keeps_each_half(void) {
    void *p = NULL;
    CHECK(map_flexible(&p, 4 * PAGE, 3, 0) == 0 && p);
    unsigned char *m = p;
    for (unsigned i = 0; i < 4; ++i) memset(m + i * PAGE, 0x11 * (i + 1), PAGE);
    CHECK(kernel_mprotect(m + 2 * PAGE, 2 * PAGE, 1) == 0); /* read-only right half: the VMA splits */
    const Vma *left = vma_at((uintptr_t)m), *right = vma_at((uintptr_t)m + 2 * PAGE);
    CHECK(left && right && left != right);
    CHECK(left->end == (uintptr_t)m + 2 * PAGE && right->start == (uintptr_t)m + 2 * PAGE);
    CHECK(right->phys == left->phys + 2 * PAGE); /* was the left half's offset */
    CHECK(release_flexible(m + 2 * PAGE, 2 * PAGE) == 0);
    /* The left half is intact: releasing the right half freed (and zeroed) its own pages. */
    CHECK(m[0] == 0x11 && m[PAGE - 1] == 0x11 && m[PAGE] == 0x22 && m[2 * PAGE - 1] == 0x22);
    /* A new flexible mapping gets other pages, never the left half's. */
    void *q = NULL;
    CHECK(map_flexible(&q, 2 * PAGE, 3, 0) == 0 && q);
    memset(q, 0xEE, 2 * PAGE);
    CHECK(m[0] == 0x11 && m[PAGE] == 0x22);
    CHECK(release_flexible(q, 2 * PAGE) == 0);
    CHECK(release_flexible(m, 2 * PAGE) == 0);
}

/* Released pages read as zero when they are handed out again (the game relies on it). */
static void released_pages_read_zero(void) {
    for (int round = 0; round < 4; ++round) {
        void *p = NULL;
        CHECK(map_flexible(&p, 8 * PAGE, 3, 0) == 0 && p);
        unsigned char *m = p;
        size_t nonzero = 0;
        for (size_t i = 0; i < 8 * PAGE; ++i) nonzero += m[i] != 0;
        CHECK(nonzero == 0);
        memset(m, 0xA5, 8 * PAGE);
        CHECK(release_flexible(m, 8 * PAGE) == 0);
    }
}

int main(int argc, char **argv) {
    const int on_map = argc > 1 && !strcmp(argv[1], "--on-map");
    if (on_map) {
#ifdef _WIN32
        _putenv("THEORDER_GUEST_COMMIT_ON_MAP=1");
#else
        setenv("THEORDER_GUEST_COMMIT_ON_MAP", "1", 1);
#endif
    }
    flexible_split_keeps_each_half();
    released_pages_read_zero();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("PASS: flexible split keeps each half's pages; released pages read zero (%s)\n",
           on_map ? "pool committed as mapped" : "pool committed at once");
    if (on_map) return 0;
    /* The same contracts with the commit-on-map pool (the error 1450 fallback). */
    char command[1024];
#ifdef _WIN32
    snprintf(command, sizeof(command), "\"\"%s\" --on-map\"", argv[0]);
#else
    snprintf(command, sizeof(command), "\"%s\" --on-map", argv[0]);
#endif
    return system(command) != 0;
}
