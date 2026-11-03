/*
 * x87_host.c
 *
 * Runs one x87 instruction on the x87 unit of the host (a real x86 processor,
 * or Rosetta 2 on an Apple silicon Mac) from a given state, for
 * tests/semantics/tools/x87_host.py.
 *
 * Build:  cc -O1 -o x87_host x87_host.c            (an x86-64 host)
 *         clang -arch x86_64 -O1 -o x87_host x87_host.c   (macOS on arm64: runs under Rosetta)
 *
 * Input, one line per run on stdin (hexadecimal numbers):
 *   code=<hex bytes of the instruction, its memory operand is [rdi]>
 *   fcw=.. fsw=.. ftw=<abridged tag word> eflags=.. eax=.. mxcsr=.. xmm0=<32 hex digits>..xmm7=
 *   r0=<20 hex digits>..r7=..  (the physical registers)
 *   mem=<hex bytes of the scratch area rdi points to, 16 byte aligned>
 * Output, one line:
 *   fcw=.. fsw=.. ftw=.. eflags=.. eax=.. r0=..r7=.. mxcsr=.. xmm0=..xmm7=.. mem=<hex bytes>
 * Or "ERR <text>" (a signal: an unmasked exception, an invalid instruction).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/mman.h>

#define MAX_LINE 8192
#define SCRATCH_SIZE 1024

static sigjmp_buf gJump;
static int gSignal;

static void onSignal(int number)
{
    gSignal = number;
    siglongjmp(gJump, 1);
}

static int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse a hex string into bytes, return the count */
static size_t parseBytes(const char* text, uint8_t* out, size_t max)
{
    size_t n = 0;
    while (text[0] != 0 && text[1] != 0 && hexValue(text[0]) >= 0 && n < max)
    {
        out[n++] = (uint8_t)(hexValue(text[0]) * 16 + hexValue(text[1]));
        text += 2;
    }
    return n;
}

/* Find "name=" in the line and return the pointer to its value, or NULL */
static const char* find(const char* line, const char* name)
{
    size_t length = strlen(name);
    const char* p = line;
    while ((p = strstr(p, name)) != NULL)
    {
        if ((p == line || p[-1] == ' ') && p[length] == '=')
            return p + length + 1;
        p += length;
    }
    return NULL;
}

static uint64_t number(const char* line, const char* name, uint64_t defaultValue)
{
    const char* p = find(line, name);
    return p ? strtoull(p, NULL, 16) : defaultValue;
}

/* An 80 bit register given as 20 hex digits, high digits first */
static void parseRegister(const char* text, uint8_t* out)
{
    uint8_t big[10];
    int i;
    parseBytes(text, big, 10);
    for (i = 0; i < 10; i++)
        out[i] = big[9 - i];
}

static void printRegister(const uint8_t* in)
{
    int i;
    for (i = 9; i >= 0; i--)
        printf("%02x", in[i]);
}

int main(void)
{
    static char line[MAX_LINE];
    uint8_t* page = (uint8_t*)mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                                   MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t* scratch;
    uint8_t* images;
    struct sigaction action;
    int signals[] = { SIGFPE, SIGILL, SIGSEGV, SIGBUS, SIGTRAP };
    unsigned i;

    if (page == MAP_FAILED)
    {
        printf("ERR mmap\n");
        return 1;
    }
    scratch = (uint8_t*)aligned_alloc(16, SCRATCH_SIZE);
    images = (uint8_t*)aligned_alloc(16, 2048);
    memset(&action, 0, sizeof(action));
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NODEFER;
    for (i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        sigaction(signals[i], &action, NULL);

    while (fgets(line, sizeof(line), stdin))
    {
        uint8_t code[64];
        size_t codeLength;
        const char* p;
        uint8_t* in = images;
        uint8_t* out = images + 1024;
        uint8_t stub[256];
        size_t n = 0;
        uint32_t eflags = (uint32_t)number(line, "eflags", 0x202);
        uint32_t eax = (uint32_t)number(line, "eax", 0);
        uint16_t fcw = (uint16_t)number(line, "fcw", 0x37f);
        uint16_t fsw = (uint16_t)number(line, "fsw", 0);
        uint8_t ftw = (uint8_t)number(line, "ftw", 0);
        unsigned top = (fsw >> 11) & 7;
        int r;
        void (*function)(uint8_t*, uint8_t*, uint8_t*);
        uint8_t physical[8][10];
        size_t memLength;

        p = find(line, "code");
        codeLength = p ? parseBytes(p, code, sizeof(code)) : 0;
        memset(scratch, 0, SCRATCH_SIZE);
        p = find(line, "mem");
        memLength = p ? parseBytes(p, scratch, SCRATCH_SIZE) : 0;

        /* The state as an FXSAVE image: ST(i) in stack order */
        memset(in, 0, 512);
        *(uint16_t*)(in + 0) = fcw;
        *(uint16_t*)(in + 2) = fsw;
        in[4] = ftw;
        *(uint32_t*)(in + 24) = (uint32_t)number(line, "mxcsr", 0x1f80);
        *(uint32_t*)(in + 28) = 0xffff;
        for (r = 0; r < 8; r++)
        {
            char name[8];
            sprintf(name, "xmm%d", r);
            p = find(line, name);
            if (p)
            {
                uint8_t big[16];
                int k;
                parseBytes(p, big, 16);
                for (k = 0; k < 16; k++)
                    in[160 + 16 * r + k] = big[15 - k];
            }
        }
        for (r = 0; r < 8; r++)
        {
            char name[8];
            sprintf(name, "r%d", r);
            p = find(line, name);
            memset(physical[r], 0, 10);
            if (p)
                parseRegister(p, physical[r]);
        }
        for (r = 0; r < 8; r++)
            memcpy(in + 32 + 16 * r, physical[(top + r) & 7], 10);

        /* The stub: rdi = scratch, rsi = state image, rdx = output image */
        stub[n++] = 0x0F; stub[n++] = 0xAE; stub[n++] = 0x0E;        /* fxrstor [rsi] */
        stub[n++] = 0x68; memcpy(stub + n, &eflags, 4); n += 4;       /* push imm32 */
        stub[n++] = 0x9D;                                             /* popfq */
        stub[n++] = 0xB8; memcpy(stub + n, &eax, 4); n += 4;          /* mov eax, imm32 */
        memcpy(stub + n, code, codeLength); n += codeLength;
        stub[n++] = 0x9C;                                             /* pushfq */
        stub[n++] = 0x41; stub[n++] = 0x58;                           /* pop r8 */
        stub[n++] = 0x0F; stub[n++] = 0xAE; stub[n++] = 0x02;        /* fxsave [rdx] */
        stub[n++] = 0x89; stub[n++] = 0x82; stub[n++] = 0x00; stub[n++] = 0x02; stub[n++] = 0; stub[n++] = 0; /* mov [rdx+512], eax */
        stub[n++] = 0x44; stub[n++] = 0x89; stub[n++] = 0x82; stub[n++] = 0x04; stub[n++] = 0x02; stub[n++] = 0; stub[n++] = 0; /* mov [rdx+516], r8d */
        stub[n++] = 0xC3;                                             /* ret */
        memcpy(page, stub, n);
        function = (void (*)(uint8_t*, uint8_t*, uint8_t*))page;
        memset(out, 0xEE, 600);

        gSignal = 0;
        if (sigsetjmp(gJump, 1) == 0)
        {
            function(scratch, in, out);
        } else
        {
            printf("ERR signal %d\n", gSignal);
            fflush(stdout);
            continue;
        }

        {
            uint16_t ofcw = *(uint16_t*)(out + 0);
            uint16_t ofsw = *(uint16_t*)(out + 2);
            uint8_t oftw = out[4];
            unsigned otop = (ofsw >> 11) & 7;
            uint32_t oeax = *(uint32_t*)(out + 512);
            uint32_t oeflags = *(uint32_t*)(out + 516);
            printf("fcw=%04x fsw=%04x ftw=%02x eflags=%08x eax=%08x", ofcw, ofsw, oftw, oeflags, oeax);
            for (r = 0; r < 8; r++)
            {
                /* physical register r is ST((r - top) & 7) */
                printf(" r%d=", r);
                printRegister(out + 32 + 16 * ((r - otop) & 7));
            }
            printf(" mxcsr=%08x", *(uint32_t*)(out + 24));
            for (r = 0; r < 8; r++)
            {
                int k;
                printf(" xmm%d=", r);
                for (k = 15; k >= 0; k--)
                    printf("%02x", out[160 + 16 * r + k]);
            }
            printf(" mem=");
            for (i = 0; i < memLength; i++)
                printf("%02x", scratch[i]);
            printf("\n");
            fflush(stdout);
        }
    }
    return 0;
}
