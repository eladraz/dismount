/*
 * cpuprobe.c
 *
 * Stand-alone CPUID probe. Prints the x86 features the current process can
 * execute, one line, space separated (Linux /proc/cpuinfo names).
 *
 * Built by run_compat.py with `clang -arch x86_64` and run under Rosetta on
 * arm64 macOS to learn what an x86 process on this host supports.
 *
 * Build: clang -arch x86_64 -O1 -o cpuprobe cpuprobe.c
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "cpufeatures.h"

int main(void)
{
    char buf[4096];
#if DM_CPU_X86
    if (dm_cpu_features(buf, sizeof(buf)) == 0 && buf[0] == 0)
        printf("none (no CPUID)\n");
    else
        printf("%s\n", buf);
#else
    dm_cpu_features(buf, sizeof(buf));
    printf("none (non-x86 host: unknown)\n");
#endif
    return 0;
}
