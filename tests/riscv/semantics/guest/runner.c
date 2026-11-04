/*
 * runner.c
 *
 * The bare metal guest of the QEMU oracle of the RISC-V semantics tests (see
 * tests/riscv/semantics/TEST_PLAN.md). It runs in M-mode on the QEMU "virt"
 * machine (RV32 or RV64, the same source), reads a batch of test records from
 * memory (loaded by `-device loader` at INPUT_PA), runs each instruction in
 * U-mode from the registers of the record and writes one result record per
 * test to OUTPUT_PA. At the end the results are written to a host file with
 * the semihosting call SYS_WRITE and the machine is powered off.
 *
 *   - Paging (Sv39 / Sv32) is on for the U-mode: the code region (3 pages
 *     around the page of the pc, zero bytes but the instruction and an ecall
 *     which follows it) and the data window are mapped, nothing else. An
 *     access outside faults (cause 13 / 15), a jump outside is a fetch fault.
 *   - The instruction bytes are followed by `ecall`: reaching it (mcause 8,
 *     mepc = pc + length) is "fell through".
 *   - A timer interrupt breaks the endless loops (mcause = interrupt | 7).
 *   - The window is restored from a pristine copy after each test; the bytes
 *     the instruction wrote are reported.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
typedef unsigned long xlen_t;
typedef unsigned long long u64;
typedef unsigned int u32;
typedef unsigned char u8;
#if __riscv_xlen == 64
typedef unsigned long pte_t;
#else
typedef unsigned int pte_t;
#endif

#ifdef SPIKE
/* Spike: --kernel loads the input flat image at the memory base + 0x200000 (RV64) / 0x400000 (RV32);
 * the results leave through the HTIF syscall proxy and the machine stops with tohost */
#if __riscv_xlen == 64
#define INPUT_PA     0x80200000UL
#else
#define INPUT_PA     0x80400000UL
#endif
#else
#define INPUT_PA     0x90000000UL
#endif
#define OUTPUT_PA    0xA0000000UL
#define CODE_PA      0x84000000UL        /* 3 pages */
#define WIN_PA       0x84100000UL
#define PRISTINE_PA  0x84200000UL
#define SS_VA        0x80200000UL        /* the shadow stack page (Zicfiss), after the window in the offsets */
#define CLINT_MTIMECMP 0x02004000UL
#define CLINT_MTIME    0x0200BFF8UL
#define TEST_FINISHER  0x00100000UL
#define TIMER_TICKS    1000000UL         /* 100 ms of the 10 MHz timer */
#define MAGIC 0x31545652UL               /* 'RVT1' */

/* The page table flags */
#define PTE_V 0x01
#define PTE_R 0x02
#define PTE_W 0x04
#define PTE_X 0x08
#define PTE_U 0x10
#define PTE_A 0x40
#define PTE_D 0x80

struct header {
    u32 magic, count, xlen, vlenb, windowBase, windowSize, flags, pad;
    char outPath[224];
};

/* A test record of the host. Followed by v[32 * vlenb] and the memory runs */
struct record {
    u32 totalLength, insnLength;
    u8 insn[48];
    u64 pc, vtype, vl, vstart, fcsr, vcsr, ssp, nmem, elp;
    u64 x[32], f[32];
};

/* A result record. The csr fields precede x (the trap handler writes them at
 * fixed offsets before x[0]); v follows f, then nmem and the changed bytes */
struct output {
    u32 totalLength, nmem;
    u64 mcause, mepc, mtval, vstart, vl, vtype, fcsr, vcsr, ssp, mstatus;
    u64 x[32], f[32];
};

extern void enter_user(const u64* xs, u64* outx);
extern void load_state(const u64* f, const u8* v, unsigned long vlenb);
extern void set_vtype(unsigned long avl, unsigned long vtype);
extern void trap_entry(void);
extern u32 cfi_enabled;
extern char _pt_start[];

#define CSRW(n, v) __asm__ volatile("csrw " #n ", %0" ::"rK"(v))
#define CSRR(n) ({ unsigned long _v; __asm__ volatile("csrr %0, " #n : "=r"(_v)); _v; })

static unsigned char* ptNext;
static pte_t* ptRoot;
static pte_t* codeTable;
static unsigned long previousCodePage = ~0UL;

static void semihost_write(const char* path, const void* data, unsigned long length)
{
    static unsigned long block[3];
    unsigned long arg[3];
    unsigned long fd;
    unsigned long n = 0;
    while (path[n])
    {
        n++;
    }
    arg[0] = (unsigned long)path;
    arg[1] = 5;                                  /* "wb" */
    arg[2] = n;
    __asm__ volatile(".balign 16\n .option push\n .option norvc\n"
                     "slli zero, zero, 0x1f\n ebreak\n srai zero, zero, 7\n .option pop\n"
                     : "+r"(arg[0]) : "r"((unsigned long)1), "r"(arg) : "memory");
    /* (the result is returned in a0, which the constraints above don't read back:
        redo it with explicit registers) */
    (void)block;
    (void)fd;
    (void)length;
    (void)data;
}

#ifdef SPIKE
volatile u64 tohost __attribute__((section(".tohost"), aligned(64)));
volatile u64 fromhost __attribute__((section(".tohost"), aligned(64)));

static long htif_syscall(unsigned long number, unsigned long a0, unsigned long a1, unsigned long a2)
{
    static volatile u64 magic[8] __attribute__((aligned(64)));
    magic[0] = number;
    magic[1] = a0;
    magic[2] = a1;
    magic[3] = a2;
    __asm__ volatile("fence" ::: "memory");
    tohost = (u64)(unsigned long)magic;
    while (fromhost == 0)
    {
    }
    fromhost = 0;
    __asm__ volatile("fence" ::: "memory");
    return (long)magic[0];
}

static void write_results(const char* path, const void* data, unsigned long length)
{
    unsigned long n = 0;
    long fd;
    unsigned long done = 0;
    while (path[n])
    {
        n++;
    }
    /* openat(AT_FDCWD (-100), path, O_WRONLY | O_CREAT | O_TRUNC (the flags of the host: macOS), 0644) */
    {
        static volatile u64 magic[8] __attribute__((aligned(64)));
        magic[0] = 56;
        magic[1] = (u64)(long long)-100;
        magic[2] = (u64)(unsigned long)path;
        magic[3] = n + 1;
        magic[4] = 0x0001 | 0x0200 | 0x0400;
        magic[5] = 0644;
        __asm__ volatile("fence" ::: "memory");
        tohost = (u64)(unsigned long)magic;
        while (fromhost == 0)
        {
        }
        fromhost = 0;
        __asm__ volatile("fence" ::: "memory");
        fd = (long)magic[0];
    }
    while (done < length)
    {
        long w = htif_syscall(64, (unsigned long)fd, (unsigned long)data + done, length - done);
        if (w <= 0)
        {
            break;
        }
        done += (unsigned long)w;
    }
    htif_syscall(57, (unsigned long)fd, 0, 0);
}

static void power_off(void)
{
    tohost = 1;                                  /* exit code 0 */
    for (;;)
    {
    }
}
#else
static long semihost(unsigned long op, unsigned long* param)
{
    register unsigned long a0 __asm__("a0") = op;
    register unsigned long a1 __asm__("a1") = (unsigned long)param;
    __asm__ volatile(".balign 16\n .option push\n .option norvc\n"
                     "slli zero, zero, 0x1f\n ebreak\n srai zero, zero, 7\n .option pop\n"
                     : "+r"(a0) : "r"(a1) : "memory");
    return (long)a0;
}

static void write_results(const char* path, const void* data, unsigned long length)
{
    unsigned long param[3];
    unsigned long n = 0;
    long fd;
    while (path[n])
    {
        n++;
    }
    param[0] = (unsigned long)path;
    param[1] = 5;                                /* "wb" */
    param[2] = n;
    fd = semihost(0x01, param);                  /* SYS_OPEN */
    param[0] = (unsigned long)fd;
    param[1] = (unsigned long)data;
    param[2] = length;
    semihost(0x05, param);                       /* SYS_WRITE */
    param[0] = (unsigned long)fd;
    semihost(0x02, param);                       /* SYS_CLOSE */
}

static void power_off(void)
{
    *(volatile u32*)TEST_FINISHER = 0x5555;
    for (;;)
    {
    }
}
#endif

static unsigned char* allocTable(void)
{
    unsigned char* t = ptNext;
    unsigned long i;
    for (i = 0; i < 4096; i++)
    {
        t[i] = 0;
    }
    ptNext += 4096;
    return t;
}

/* Returns the pointer to the leaf entry of the virtual address, creating the tables */
static pte_t* walk(xlen_t va)
{
    pte_t* t = ptRoot;
#if __riscv_xlen == 64
    int level;
    for (level = 2; level > 0; level--)
    {
        unsigned long idx = (va >> (12 + 9 * level)) & 511;
        if (!(t[idx] & PTE_V))
        {
            t[idx] = (((xlen_t)allocTable() >> 12) << 10) | PTE_V;
        }
        t = (pte_t*)((t[idx] >> 10) << 12);
    }
    return &t[(va >> 12) & 511];
#else
    unsigned long idx = (va >> 22) & 1023;
    if (!(t[idx] & PTE_V))
    {
        t[idx] = (((xlen_t)allocTable() >> 12) << 10) | PTE_V;
    }
    t = (pte_t*)((t[idx] >> 10) << 12);
    return &t[(va >> 12) & 1023];
#endif
}

static void mapPage(xlen_t va, xlen_t pa, pte_t flags)
{
    *walk(va) = (pte_t)(((pa >> 12) << 10) | flags | PTE_A | PTE_D);
}

static void setTimer(u64 value)
{
    volatile u32* cmp = (volatile u32*)CLINT_MTIMECMP;
    cmp[1] = 0xFFFFFFFFu;
    cmp[0] = (u32)value;
    cmp[1] = (u32)(value >> 32);
}

static u64 readTime(void)
{
    volatile u32* t = (volatile u32*)CLINT_MTIME;
    u32 hi, lo;
    do
    {
        hi = t[1];
        lo = t[0];
    } while (hi != t[1]);
    return ((u64)hi << 32) | lo;
}

static void setupCodeMapping(unsigned long pcPage)
{
    unsigned long i;
    if (pcPage == previousCodePage)
    {
        return;
    }
    if (previousCodePage != ~0UL)
    {
        for (i = 0; i < 3; i++)
        {
            *walk((previousCodePage + i - 1) << 12) = 0;
        }
    }
    for (i = 0; i < 3; i++)
    {
        mapPage((pcPage + i - 1) << 12, CODE_PA + (i << 12), PTE_V | PTE_R | PTE_W | PTE_X | PTE_U);
    }
    previousCodePage = pcPage;
    __asm__ volatile("sfence.vma" ::: "memory");
}

int main(void)
{
    struct header* h = (struct header*)INPUT_PA;
    const u8* cursor = (const u8*)(INPUT_PA + sizeof(struct header));
    u8* out = (u8*)OUTPUT_PA;
    unsigned long vlenb, windowSize, windowBase, i, test;
    unsigned long satp, regionSize;

    if (h->magic != MAGIC)
    {
        power_off();
    }
    vlenb = h->vlenb;
    windowSize = h->windowSize;
    windowBase = h->windowBase;
    cfi_enabled = h->flags & 1;
    /* The window, and the shadow stack page after it (the offsets from windowSize) with Zicfiss */
    regionSize = windowSize + (cfi_enabled ? 4096 : 0);

    /* The page tables */
    ptNext = (unsigned char*)_pt_start;
    ptRoot = (pte_t*)allocTable();
    for (i = 0; i < windowSize; i += 4096)
    {
        mapPage(windowBase + i, WIN_PA + i, PTE_V | PTE_R | PTE_W | PTE_U);
    }
    if (cfi_enabled)
    {
        /* a shadow stack page: readable by nothing but the shadow stack instructions (R = 0, W = 1) */
        mapPage(SS_VA, WIN_PA + windowSize, PTE_V | PTE_W | PTE_U);
    }
    /* The pristine window: byte(A) = ((uint32)(A * 2654435761) >> 24) */
    for (i = 0; i < regionSize; i++)
    {
        unsigned long va = i < windowSize ? windowBase + i : SS_VA + (i - windowSize);
        u8 b = (u8)(((u32)((u32)va * 2654435761u)) >> 24);
        ((u8*)PRISTINE_PA)[i] = b;
        ((u8*)WIN_PA)[i] = b;
    }
    for (i = 0; i < 3 * 4096; i++)
    {
        ((u8*)CODE_PA)[i] = 0;
    }
#if __riscv_xlen == 64
    satp = (8UL << 60) | ((xlen_t)ptRoot >> 12);
#else
    satp = (1UL << 31) | ((xlen_t)ptRoot >> 12);
#endif
    {
        unsigned long ms = (3UL << 13) | (3UL << 9);
        CSRW(mstatus, ms);
    }
    CSRW(satp, satp);
    CSRW(mtvec, (xlen_t)trap_entry);
    CSRW(medeleg, 0);
    CSRW(0x303, 0);                              /* mideleg */
    CSRW(mie, 0x80);                             /* MTIE */
    CSRW(0x306, 0xFFFFFFFFUL);                   /* mcounteren */
    CSRW(0x106, 0xFFFFFFFFUL);                   /* scounteren */
    CSRW(0x320, 0);                              /* mcountinhibit */
    CSRW(pmpaddr0, ~0UL);
    CSRW(pmpcfg0, 0x1F);                         /* NAPOT, RWX over all of memory */
    {
        /* menvcfg / senvcfg: CBIE (4-5), CBCFE (6), CBZE (7), and the CFI enables LPE (2) SSE (3) */
        unsigned long env = 0xF0;
        if (cfi_enabled)
        {
            env |= 0x0C;
        }
        CSRW(0x30A, env);
        CSRW(0x10A, env);
    }
    setTimer(~0ULL);

    for (test = 0; test < h->count; test++)
    {
        const struct record* r = (const struct record*)cursor;
        const u8* vdata = (const u8*)(r + 1);
        const u8* runs = vdata + 32 * vlenb;
        struct output* o = (struct output*)out;
        u8* vout = (u8*)(o + 1);
        u32* memOut;
        unsigned long pcPage = (unsigned long)(r->pc >> 12);
        unsigned long codeOffset = 4096 + (unsigned long)(r->pc & 0xFFF);
        u8* code = (u8*)(CODE_PA + codeOffset);
        unsigned long nmem = 0, run;
        const u8* p;
        unsigned long mstatus;

        /* The initial memory */
        p = runs;
        for (run = 0; run < r->nmem; run++)
        {
            u32 off = ((const u32*)p)[0];
            u32 len = ((const u32*)p)[1];
            u32 k;
            for (k = 0; k < len; k++)
            {
                ((u8*)WIN_PA)[off + k] = p[8 + k];
            }
            p += 8 + ((len + 7) & ~7u);
        }
        /* The code */
        setupCodeMapping(pcPage);
        for (i = 0; i < r->insnLength; i++)
        {
            code[i] = r->insn[i];
        }
        code[r->insnLength + 0] = 0x73;          /* ecall */
        code[r->insnLength + 1] = 0x00;
        code[r->insnLength + 2] = 0x00;
        code[r->insnLength + 3] = 0x00;
        __asm__ volatile("fence.i" ::: "memory");

        /* The state */
        CSRW(0x008, 0);                          /* vstart */
        load_state(r->f, vdata, vlenb);
        CSRW(0x003, r->fcsr);
        CSRW(0x00F, r->vcsr);
        set_vtype((unsigned long)r->vl, (unsigned long)r->vtype);
        CSRW(0x008, r->vstart);
        if (cfi_enabled)
        {
            CSRW(0x011, r->ssp);
        }
        CSRW(mepc, (xlen_t)r->pc);
        mstatus = (3UL << 13) | (3UL << 9) | (1UL << 7);   /* FS, VS dirty, MPIE, MPP = U */
#if __riscv_xlen == 64
        if (r->elp)
        {
            mstatus |= 1UL << 41;                /* MPELP: ELP after the mret */
        }
        CSRW(mstatus, mstatus);
#else
        CSRW(mstatus, mstatus);
        {
            unsigned long mstatush = r->elp ? (1UL << 9) : 0;   /* MPELP */
            CSRW(0x310, mstatush);
        }
#endif
        setTimer(readTime() + TIMER_TICKS);

        o->mcause = 0;
        enter_user(r->x, o->x);
        setTimer(~0ULL);

        /* The memory: the changed bytes. The test's own initial runs first */
        memOut = (u32*)(vout + 32 * vlenb + 8);
        p = runs;
        for (run = 0; run < r->nmem; run++)
        {
            u32 off = ((const u32*)p)[0];
            u32 len = ((const u32*)p)[1];
            u32 k;
            for (k = 0; k < len; k++)
            {
                u8 now = ((u8*)WIN_PA)[off + k];
                if (now != p[8 + k])
                {
                    memOut[nmem++] = (off + k) | ((u32)now << 24);
                }
            }
            p += 8 + ((len + 7) & ~7u);
        }
        {
            unsigned long* w = (unsigned long*)WIN_PA;
            unsigned long* pr = (unsigned long*)PRISTINE_PA;
            unsigned long words = regionSize / sizeof(unsigned long);
            for (i = 0; i < words; i++)
            {
                if (w[i] != pr[i])
                {
                    unsigned long b;
                    for (b = 0; b < sizeof(unsigned long); b++)
                    {
                        unsigned long off = i * sizeof(unsigned long) + b;
                        u8 now = ((u8*)WIN_PA)[off];
                        if (now != ((u8*)PRISTINE_PA)[off])
                        {
                            /* (not in a run of the record: those were reported above) */
                            int inRun = 0;
                            p = runs;
                            for (run = 0; run < r->nmem; run++)
                            {
                                u32 roff = ((const u32*)p)[0];
                                u32 rlen = ((const u32*)p)[1];
                                if (off >= roff && off < roff + rlen)
                                {
                                    inRun = 1;
                                }
                                p += 8 + ((rlen + 7) & ~7u);
                            }
                            if (!inRun)
                            {
                                memOut[nmem++] = (u32)off | ((u32)now << 24);
                            }
                        }
                    }
                    w[i] = pr[i];
                }
            }
        }
        /* Restore the runs of the record */
        p = runs;
        for (run = 0; run < r->nmem; run++)
        {
            u32 off = ((const u32*)p)[0];
            u32 len = ((const u32*)p)[1];
            u32 k;
            for (k = 0; k < len; k++)
            {
                ((u8*)WIN_PA)[off + k] = ((u8*)PRISTINE_PA)[off + k];
            }
            p += 8 + ((len + 7) & ~7u);
        }
        /* Clean the code */
        for (i = 0; i < r->insnLength + 8; i++)
        {
            code[i] = 0;
        }
        ((u32*)(vout + 32 * vlenb))[0] = (u32)nmem;
        ((u32*)(vout + 32 * vlenb))[1] = 0;
        o->nmem = (u32)nmem;
        {
            unsigned long length = sizeof(struct output) + 32 * vlenb + 8 + 4 * nmem;
            length = (length + 7) & ~7UL;
            o->totalLength = (u32)length;
            out += length;
        }
        cursor += r->totalLength;
    }
    write_results(h->outPath, (const void*)OUTPUT_PA, (unsigned long)(out - (u8*)OUTPUT_PA));
    power_off();
    return 0;
}
