/*
 * cpufeatures.h
 *
 * CPUID decoding shared by dismount_cli (cpuinfo) and cpuprobe.c. Produces the
 * Linux /proc/cpuinfo flag names for the features the current process can
 * execute. Header-only and usable from both C and C++.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#ifndef __TBA_DISMOUNT_TESTS_IA32_CPUFEATURES_H
#define __TBA_DISMOUNT_TESTS_IA32_CPUFEATURES_H

#include <stdio.h>
#include <string.h>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#  include <intrin.h>
#  define DM_CPU_X86 1
#elif defined(__i386__) || defined(__x86_64__)
#  include <cpuid.h>
#  define DM_CPU_X86 1
#else
#  define DM_CPU_X86 0
#endif

/* Register selectors for dm_feature_t.reg */
#define DM_EAX 0
#define DM_EBX 1
#define DM_ECX 2
#define DM_EDX 3

/* Feature classes: which OS state must be enabled for the flag to be real */
#define DM_NEEDS_NONE 0
#define DM_NEEDS_AVX  1   /* XCR0 bits 1,2 */
#define DM_NEEDS_AVX512 2 /* XCR0 bits 1,2,5,6,7 */

typedef struct {
    unsigned leaf;
    unsigned subleaf;
    int reg;
    unsigned bit;
    int needs;
    const char* name;
} dm_feature_t;

static const dm_feature_t dm_feature_table[] = {
    /* CPUID.1 EDX */
    {1, 0, DM_EDX, 0,  DM_NEEDS_NONE, "fpu"},
    {1, 0, DM_EDX, 1,  DM_NEEDS_NONE, "vme"},
    {1, 0, DM_EDX, 2,  DM_NEEDS_NONE, "de"},
    {1, 0, DM_EDX, 3,  DM_NEEDS_NONE, "pse"},
    {1, 0, DM_EDX, 4,  DM_NEEDS_NONE, "tsc"},
    {1, 0, DM_EDX, 5,  DM_NEEDS_NONE, "msr"},
    {1, 0, DM_EDX, 6,  DM_NEEDS_NONE, "pae"},
    {1, 0, DM_EDX, 7,  DM_NEEDS_NONE, "mce"},
    {1, 0, DM_EDX, 8,  DM_NEEDS_NONE, "cx8"},
    {1, 0, DM_EDX, 9,  DM_NEEDS_NONE, "apic"},
    {1, 0, DM_EDX, 11, DM_NEEDS_NONE, "sep"},
    {1, 0, DM_EDX, 12, DM_NEEDS_NONE, "mtrr"},
    {1, 0, DM_EDX, 13, DM_NEEDS_NONE, "pge"},
    {1, 0, DM_EDX, 14, DM_NEEDS_NONE, "mca"},
    {1, 0, DM_EDX, 15, DM_NEEDS_NONE, "cmov"},
    {1, 0, DM_EDX, 16, DM_NEEDS_NONE, "pat"},
    {1, 0, DM_EDX, 17, DM_NEEDS_NONE, "pse36"},
    {1, 0, DM_EDX, 19, DM_NEEDS_NONE, "clflush"},
    {1, 0, DM_EDX, 23, DM_NEEDS_NONE, "mmx"},
    {1, 0, DM_EDX, 24, DM_NEEDS_NONE, "fxsr"},
    {1, 0, DM_EDX, 25, DM_NEEDS_NONE, "sse"},
    {1, 0, DM_EDX, 26, DM_NEEDS_NONE, "sse2"},
    {1, 0, DM_EDX, 28, DM_NEEDS_NONE, "ht"},
    /* CPUID.1 ECX */
    {1, 0, DM_ECX, 0,  DM_NEEDS_NONE, "pni"},
    {1, 0, DM_ECX, 1,  DM_NEEDS_NONE, "pclmulqdq"},
    {1, 0, DM_ECX, 3,  DM_NEEDS_NONE, "monitor"},
    {1, 0, DM_ECX, 5,  DM_NEEDS_NONE, "vmx"},
    {1, 0, DM_ECX, 7,  DM_NEEDS_NONE, "est"},
    {1, 0, DM_ECX, 9,  DM_NEEDS_NONE, "ssse3"},
    {1, 0, DM_ECX, 12, DM_NEEDS_AVX,  "fma"},   /* VEX encoded: needs YMM state */
    {1, 0, DM_ECX, 13, DM_NEEDS_NONE, "cx16"},
    {1, 0, DM_ECX, 15, DM_NEEDS_NONE, "pdcm"},
    {1, 0, DM_ECX, 17, DM_NEEDS_NONE, "pcid"},
    {1, 0, DM_ECX, 18, DM_NEEDS_NONE, "dca"},
    {1, 0, DM_ECX, 19, DM_NEEDS_NONE, "sse4_1"},
    {1, 0, DM_ECX, 20, DM_NEEDS_NONE, "sse4_2"},
    {1, 0, DM_ECX, 21, DM_NEEDS_NONE, "x2apic"},
    {1, 0, DM_ECX, 22, DM_NEEDS_NONE, "movbe"},
    {1, 0, DM_ECX, 23, DM_NEEDS_NONE, "popcnt"},
    {1, 0, DM_ECX, 25, DM_NEEDS_NONE, "aes"},
    {1, 0, DM_ECX, 26, DM_NEEDS_NONE, "xsave"},
    {1, 0, DM_ECX, 28, DM_NEEDS_AVX,  "avx"},
    {1, 0, DM_ECX, 29, DM_NEEDS_AVX,  "f16c"},
    {1, 0, DM_ECX, 30, DM_NEEDS_NONE, "rdrand"},
    /* CPUID.7.0 EBX */
    {7, 0, DM_EBX, 0,  DM_NEEDS_NONE,   "fsgsbase"},
    {7, 0, DM_EBX, 2,  DM_NEEDS_NONE,   "sgx"},
    {7, 0, DM_EBX, 3,  DM_NEEDS_NONE,   "bmi1"},
    {7, 0, DM_EBX, 4,  DM_NEEDS_NONE,   "hle"},
    {7, 0, DM_EBX, 5,  DM_NEEDS_AVX,    "avx2"},
    {7, 0, DM_EBX, 7,  DM_NEEDS_NONE,   "smep"},
    {7, 0, DM_EBX, 8,  DM_NEEDS_NONE,   "bmi2"},
    {7, 0, DM_EBX, 9,  DM_NEEDS_NONE,   "erms"},
    {7, 0, DM_EBX, 10, DM_NEEDS_NONE,   "invpcid"},
    {7, 0, DM_EBX, 11, DM_NEEDS_NONE,   "rtm"},
    {7, 0, DM_EBX, 14, DM_NEEDS_NONE,   "mpx"},
    {7, 0, DM_EBX, 16, DM_NEEDS_AVX512, "avx512f"},
    {7, 0, DM_EBX, 17, DM_NEEDS_AVX512, "avx512dq"},
    {7, 0, DM_EBX, 18, DM_NEEDS_NONE,   "rdseed"},
    {7, 0, DM_EBX, 19, DM_NEEDS_NONE,   "adx"},
    {7, 0, DM_EBX, 20, DM_NEEDS_NONE,   "smap"},
    {7, 0, DM_EBX, 21, DM_NEEDS_AVX512, "avx512ifma"},
    {7, 0, DM_EBX, 23, DM_NEEDS_NONE,   "clflushopt"},
    {7, 0, DM_EBX, 24, DM_NEEDS_NONE,   "clwb"},
    {7, 0, DM_EBX, 26, DM_NEEDS_AVX512, "avx512pf"},
    {7, 0, DM_EBX, 27, DM_NEEDS_AVX512, "avx512er"},
    {7, 0, DM_EBX, 28, DM_NEEDS_AVX512, "avx512cd"},
    {7, 0, DM_EBX, 29, DM_NEEDS_NONE,   "sha_ni"},
    {7, 0, DM_EBX, 30, DM_NEEDS_AVX512, "avx512bw"},
    {7, 0, DM_EBX, 31, DM_NEEDS_AVX512, "avx512vl"},
    /* CPUID.7.0 ECX */
    {7, 0, DM_ECX, 1,  DM_NEEDS_AVX512, "avx512vbmi"},
    {7, 0, DM_ECX, 2,  DM_NEEDS_NONE,   "umip"},
    {7, 0, DM_ECX, 3,  DM_NEEDS_NONE,   "pku"},
    {7, 0, DM_ECX, 4,  DM_NEEDS_NONE,   "ospke"},
    {7, 0, DM_ECX, 5,  DM_NEEDS_NONE,   "waitpkg"},
    {7, 0, DM_ECX, 6,  DM_NEEDS_AVX512, "avx512_vbmi2"},
    {7, 0, DM_ECX, 8,  DM_NEEDS_NONE,   "gfni"},
    {7, 0, DM_ECX, 9,  DM_NEEDS_AVX,    "vaes"},
    {7, 0, DM_ECX, 10, DM_NEEDS_AVX,    "vpclmulqdq"},
    {7, 0, DM_ECX, 11, DM_NEEDS_AVX512, "avx512_vnni"},
    {7, 0, DM_ECX, 12, DM_NEEDS_AVX512, "avx512_bitalg"},
    {7, 0, DM_ECX, 14, DM_NEEDS_AVX512, "avx512_vpopcntdq"},
    {7, 0, DM_ECX, 22, DM_NEEDS_NONE,   "rdpid"},
    {7, 0, DM_ECX, 25, DM_NEEDS_NONE,   "cldemote"},
    {7, 0, DM_ECX, 27, DM_NEEDS_NONE,   "movdiri"},
    {7, 0, DM_ECX, 28, DM_NEEDS_NONE,   "movdir64b"},
    /* CPUID.7.0 EDX */
    {7, 0, DM_EDX, 2,  DM_NEEDS_AVX512, "avx512_4vnniw"},
    {7, 0, DM_EDX, 3,  DM_NEEDS_AVX512, "avx512_4fmaps"},
    {7, 0, DM_EDX, 4,  DM_NEEDS_NONE,   "fsrm"},
    {7, 0, DM_EDX, 8,  DM_NEEDS_AVX512, "avx512_vp2intersect"},
    {7, 0, DM_EDX, 10, DM_NEEDS_NONE,   "md_clear"},
    {7, 0, DM_EDX, 14, DM_NEEDS_NONE,   "serialize"},
    {7, 0, DM_EDX, 22, DM_NEEDS_NONE,   "amx_bf16"},
    {7, 0, DM_EDX, 23, DM_NEEDS_AVX512, "avx512_fp16"},
    {7, 0, DM_EDX, 24, DM_NEEDS_NONE,   "amx_tile"},
    {7, 0, DM_EDX, 25, DM_NEEDS_NONE,   "amx_int8"},
    /* CPUID.80000001 ECX / EDX */
    {0x80000001u, 0, DM_ECX, 0,  DM_NEEDS_NONE, "lahf_lm"},
    {0x80000001u, 0, DM_ECX, 1,  DM_NEEDS_NONE, "cmp_legacy"},
    {0x80000001u, 0, DM_ECX, 2,  DM_NEEDS_NONE, "svm"},
    {0x80000001u, 0, DM_ECX, 5,  DM_NEEDS_NONE, "abm"},
    {0x80000001u, 0, DM_ECX, 6,  DM_NEEDS_NONE, "sse4a"},
    {0x80000001u, 0, DM_ECX, 8,  DM_NEEDS_NONE, "3dnowprefetch"},
    {0x80000001u, 0, DM_ECX, 11, DM_NEEDS_AVX,  "xop"},
    {0x80000001u, 0, DM_ECX, 16, DM_NEEDS_AVX,  "fma4"},
    {0x80000001u, 0, DM_ECX, 21, DM_NEEDS_NONE, "tbm"},
    {0x80000001u, 0, DM_EDX, 11, DM_NEEDS_NONE, "syscall"},
    {0x80000001u, 0, DM_EDX, 20, DM_NEEDS_NONE, "nx"},
    {0x80000001u, 0, DM_EDX, 22, DM_NEEDS_NONE, "mmxext"},
    {0x80000001u, 0, DM_EDX, 25, DM_NEEDS_NONE, "fxsr_opt"},
    {0x80000001u, 0, DM_EDX, 26, DM_NEEDS_NONE, "pdpe1gb"},
    {0x80000001u, 0, DM_EDX, 27, DM_NEEDS_NONE, "rdtscp"},
    {0x80000001u, 0, DM_EDX, 29, DM_NEEDS_NONE, "lm"},
    {0x80000001u, 0, DM_EDX, 30, DM_NEEDS_NONE, "3dnowext"},
    {0x80000001u, 0, DM_EDX, 31, DM_NEEDS_NONE, "3dnow"}
};

#define DM_FEATURE_COUNT ((int)(sizeof(dm_feature_table) / sizeof(dm_feature_table[0])))

#if DM_CPU_X86

/* Returns 1 if the cpu/OS supports the given feature class */
static int dm_os_state_ok(unsigned xcr0, int needs)
{
    unsigned avxMask = 0x6;      /* XMM + YMM state */
    unsigned avx512Mask = 0xE0;  /* opmask + ZMM_Hi256 + Hi16_ZMM */
    if (needs == DM_NEEDS_NONE)
        return 1;
    if ((xcr0 & avxMask) != avxMask)
        return 0;
    if (needs == DM_NEEDS_AVX512 && (xcr0 & avx512Mask) != avx512Mask)
        return 0;
    return 1;
}

static void dm_cpuid(unsigned leaf, unsigned subleaf, unsigned regs[4])
{
#if defined(_MSC_VER)
    int r[4];
    __cpuidex(r, (int)leaf, (int)subleaf);
    regs[0] = (unsigned)r[0]; regs[1] = (unsigned)r[1];
    regs[2] = (unsigned)r[2]; regs[3] = (unsigned)r[3];
#else
    unsigned a, b, c, d;
    __cpuid_count(leaf, subleaf, a, b, c, d);
    regs[0] = a; regs[1] = b; regs[2] = c; regs[3] = d;
#endif
}

static unsigned dm_max_leaf(void)
{
    unsigned regs[4];
    dm_cpuid(0, 0, regs);
    return regs[0];
}

static unsigned dm_xcr0(void)
{
#if defined(_MSC_VER)
    return (unsigned)_xgetbv(0);
#else
    unsigned lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return lo;
#endif
}

/*
 * Writes the space-separated feature list into buf. Returns the number of
 * features written (0 on a CPU without CPUID).
 */
static int dm_cpu_features(char* buf, size_t size)
{
    unsigned maxLeaf, regs1[4], xcr0 = 0;
    int count = 0;
    size_t used = 0;
    int i;

    if (size == 0)
        return 0;
    buf[0] = 0;
    maxLeaf = dm_max_leaf();
    if (maxLeaf < 1)
        return 0;
    dm_cpuid(1, 0, regs1);
    /* XGETBV is only legal when the OS enabled XSAVE/OSXSAVE */
    if (regs1[2] & (1u << 27))
        xcr0 = dm_xcr0();

    for (i = 0; i < DM_FEATURE_COUNT; i++)
    {
        const dm_feature_t* f = &dm_feature_table[i];
        unsigned regs[4];
        size_t len;
        if (f->leaf >= 0x80000000u) {
            dm_cpuid(0x80000000u, 0, regs);
            if (regs[0] < f->leaf)
                continue;
        } else if (f->leaf > maxLeaf) {
            continue;
        }
        dm_cpuid(f->leaf, f->subleaf, regs);
        if (!((regs[f->reg] >> f->bit) & 1))
            continue;
        if (!dm_os_state_ok(xcr0, f->needs))
            continue;
        len = strlen(f->name);
        if (used + len + 2 > size)
            break;
        if (used)
            buf[used++] = ' ';
        memcpy(buf + used, f->name, len);
        used += len;
        buf[used] = 0;
        count++;
    }
    return count;
}

#else /* !DM_CPU_X86 */

static int dm_cpu_features(char* buf, size_t size)
{
    if (size)
        buf[0] = 0;
    return 0;
}

#endif /* DM_CPU_X86 */

#endif /* __TBA_DISMOUNT_TESTS_IA32_CPUFEATURES_H */
