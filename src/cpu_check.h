/* The guest code (eboot.bin, libc.prx) is compiled for the PS4's Jaguar CPU and runs on the host
 * CPU as it is. A host without one of Jaguar's instruction sets stops with an illegal
 * instruction somewhere inside the game (#33, #66); this check names the missing sets at start. */
#ifndef THEORDER_CPU_CHECK_H
#define THEORDER_CPU_CHECK_H
#include <stddef.h>
#include <stdint.h>

/* CPUID words: leaf 1 ECX, leaf 7 subleaf 0 EBX, leaf 0x80000001 ECX. */
typedef struct { uint32_t leaf1_ecx, leaf7_ebx, ext1_ecx; } CpuFeatureWords;

CpuFeatureWords cpu_feature_words(void);
enum { CPU_SET_MOVBE = 1u << 0, CPU_SET_BMI1 = 1u << 1, CPU_SET_LZCNT = 1u << 2, CPU_SET_F16C = 1u << 3 };
/* Fatal sets the host lacks (SSE4.2, POPCNT, AVX), comma separated in out; returns how many. */
int cpu_missing_fatal(CpuFeatureWords words, char *out, size_t size);
/* Rewritable sets the host lacks (MOVBE, F16C, BMI1, LZCNT): CPU_SET_* mask; names in out. */
unsigned cpu_missing_rewritable(CpuFeatureWords words, char *out, size_t size);
/* A comma separated list of rewritable set names (THEORDER_ISA_REWRITE=MOVBE,BMI1,LZCNT): sets
 * *mask to their CPU_SET_* bits and writes their names to out; 0 (mask untouched) for an empty
 * list or an unknown name. */
int cpu_parse_rewrite_sets(const char *list, unsigned *mask, char *out, size_t size);

#endif
