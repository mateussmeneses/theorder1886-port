#include "cpu_check.h"
#include <cpuid.h>
#include <stdio.h>
#include <string.h>

CpuFeatureWords cpu_feature_words(void) {
    CpuFeatureWords words = {0, 0, 0};
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) words.leaf1_ecx = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) words.leaf7_ebx = b;
    if (__get_cpuid(0x80000001, &a, &b, &c, &d)) words.ext1_ecx = c;
    (void)a; (void)d;
    return words;
}

/* Jaguar's sets beyond the x86-64 baseline that Intel CPUs also have (SSE4A is left out: the
 * game runs on Intel). Each one arrived on Intel with Haswell at the latest. A missing fatal set
 * stops the game; the rewritable ones, which Sandy and Ivy Bridge lack, can be emulated. */
enum { LEAF1, LEAF7, EXT1 };
typedef struct { const char *name; int word; uint32_t bit; unsigned set; } CpuSet;
static const CpuSet fatal_sets[] = {
    {"SSE4.2", LEAF1, 1u << 20, 0}, {"POPCNT", LEAF1, 1u << 23, 0}, {"AVX", LEAF1, 1u << 28, 0},
};
static const CpuSet rewritable_sets[] = {
    {"MOVBE", LEAF1, 1u << 22, CPU_SET_MOVBE}, {"F16C", LEAF1, 1u << 29, CPU_SET_F16C},
    {"BMI1", LEAF7, 1u << 3, CPU_SET_BMI1},    {"LZCNT", EXT1, 1u << 5, CPU_SET_LZCNT},
};

/* Names the missing sets, comma separated in out; returns how many, and their CPU_SET_* bits in *mask. */
static int missing_sets(CpuFeatureWords words, const CpuSet *sets, size_t count, char *out, size_t size,
                        unsigned *mask) {
    const uint32_t values[] = {words.leaf1_ecx, words.leaf7_ebx, words.ext1_ecx};
    int missing = 0;
    size_t used = 0;
    *mask = 0;
    if (size) out[0] = 0;
    for (size_t i = 0; i < count; ++i) {
        if (values[sets[i].word] & sets[i].bit) continue;
        if (used < size) {
            const int n = snprintf(out + used, size - used, "%s%s", missing ? ", " : "", sets[i].name);
            used = n < 0 ? size : used + (size_t)n;
            if (used >= size) used = size - 1;
        }
        *mask |= sets[i].set;
        ++missing;
    }
    return missing;
}

int cpu_missing_fatal(CpuFeatureWords words, char *out, size_t size) {
    unsigned mask;
    return missing_sets(words, fatal_sets, sizeof fatal_sets / sizeof *fatal_sets, out, size, &mask);
}

unsigned cpu_missing_rewritable(CpuFeatureWords words, char *out, size_t size) {
    unsigned mask;
    missing_sets(words, rewritable_sets, sizeof rewritable_sets / sizeof *rewritable_sets, out, size, &mask);
    return mask;
}

/* THEORDER_ISA_REWRITE as a list of set names ("MOVBE,BMI1,LZCNT" or "MOVBE+BMI1+LZCNT", case and
 * spaces ignored):
 * rewrite as if the host lacked them, to exercise an older CPU's path on a newer one. */
int cpu_parse_rewrite_sets(const char *list, unsigned *mask, char *out, size_t size) {
    unsigned parsed = 0;
    const char *p = list;
    while (p && *p) {
        while (*p == ' ' || *p == ',' || *p == '+') ++p;
        const char *end = p;
        while (*end && *end != ',' && *end != '+') ++end;
        size_t len = (size_t)(end - p);
        while (len && p[len - 1] == ' ') --len;
        if (len) {
            unsigned found = 0;
            for (size_t i = 0; i < sizeof rewritable_sets / sizeof *rewritable_sets; ++i) {
                const char *name = rewritable_sets[i].name;
                if (strlen(name) != len) continue;
                size_t k = 0;
                while (k < len && (p[k] | 0x20) == (name[k] | 0x20)) ++k;
                if (k == len) found = rewritable_sets[i].set;
            }
            if (!found) return 0;
            parsed |= found;
        }
        p = end;
    }
    if (!parsed) return 0;
    /* Names in the table's order, the same text cpu_missing_rewritable would print. */
    size_t used = 0;
    if (size) out[0] = 0;
    for (size_t i = 0, n = 0; i < sizeof rewritable_sets / sizeof *rewritable_sets; ++i) {
        if (!(parsed & rewritable_sets[i].set) || used >= size) continue;
        const int w = snprintf(out + used, size - used, "%s%s", n++ ? ", " : "", rewritable_sets[i].name);
        used = w < 0 ? size : used + (size_t)w;
        if (used >= size) used = size - 1;
    }
    *mask = parsed;
    return 1;
}
