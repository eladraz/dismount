/*
 * host_arm64.c
 *
 * The native oracle of the A64 semantics tests (tests/arm/semantics/TEST_PLAN.md): it executes
 * the bytes of an instruction on the host processor (an Apple M4 with Advanced SIMD, FP16,
 * dot product, bf16, i8mm and SME / SME2 in the streaming mode) from a given state and prints
 * the final state. Like tests/semantics/rosetta_probe.c for the x86 host.
 *
 *   clang -O1 -o out/host_arm64 tests/arm/semantics/host_arm64.c
 *
 * Input, one run per line, the state line of the protocol (armstate.py):
 *   <hex bytes of the code> ; x0=.. sp=.. pc=.. nzcv=.. fpcr=.. v0=<32 hex> ... mem=addr:hex,..
 *   plus sm=1 (streaming mode: smstart sm; z0-z31, p0-p15), za=1 (smstart za; za0..za63, zt0).
 * Output, one line for each input line: `OK exit=<exit> ...` or `ERR <message>`, the result
 * line of the protocol.
 *
 * How it works:
 *   - The code runs from a MAP_JIT region mapped at the pc of the state, so the pc relative
 *     instructions and the pc in the result match the other engines. The region has three
 *     pages: a prologue that loads the state, the instruction, and an epilogue (z, p, za
 *     stores of the streaming mode) followed by `brk #0xBEEF`. All the other words of the
 *     region are `brk #0x1234`, so a taken branch inside it ends the run at the target.
 *   - The state is loaded from a block in memory addressed by x28. The registers x0-x27, x29,
 *     x30, sp, nzcv, fpcr, fpsr and v0-v31 are the ones of the state. x28 (the pointer of
 *     the block) is reserved, and x18 is reserved by the platform: neither is compared by the
 *     driver, and a corpus line must not use them.
 *   - The run ends with a signal: the `brk #0xBEEF` after the instruction (exit=fall), a brk
 *     of the sled (a branch into the region), SIGSEGV / SIGBUS of an instruction fetch (a
 *     branch outside of it: the target is the pc), SIGILL (trap:undef), SIGSEGV / SIGBUS
 *     inside the instruction (trap:fault), the brk of the instruction (trap:brk). The
 *     handler (on an alternate stack) copies the registers from the ucontext and returns to
 *     the main loop with siglongjmp.
 *   - The svc instruction is not run (it would call the kernel): ERR.
 *   - A 2 second timer ends a run that loops (ERR timeout).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/ucontext.h>
#include <mach/mach.h>
#include <pthread.h>
#include <libkern/OSCacheControl.h>

#define A64_PC_DEFAULT   0x0000010000001000ULL
#define MEM_BASE         0x0000010100000000ULL
#define MEM_SIZE         0x10000
#define SVL_BYTES        64         /* the streaming vector length of the M4 (512 bits) */
#define ZA_ROWS          SVL_BYTES
#define SLED_IMM         0x1234
#define END_IMM          0x1235

/* The state block (x28 points to it while the code runs). The offsets are used by the asm */
#define OFF_X        0          /* x0..x30, 8 bytes each (x28 unused) */
#define OFF_SP       248
#define OFF_NZCV     256
#define OFF_FPCR     264
#define OFF_FPSR     272
#define OFF_SAVE     288        /* 6 * 8: scratch of the epilogue */
#define OFF_FPCR_OUT 344
#define OFF_FPSR_OUT 352
#define OFF_V        384        /* v0..v31, 16 bytes each, ends at 896 */
#define OFF_Z        1024       /* z0..z31, SVL_BYTES each, ends at 3072 */
#define OFF_P        3072       /* p0..p15, 8 bytes each (stored with the stride of the predicate length) */
#define OFF_ZOUT     4096
#define OFF_POUT     (OFF_ZOUT + 32 * SVL_BYTES)
#define OFF_ZA       8192       /* ZA_ROWS rows of SVL_BYTES */
#define OFF_ZAOUT    (OFF_ZA + ZA_ROWS * SVL_BYTES)
#define OFF_ZT0      32768
#define OFF_ZT0OUT   (OFF_ZT0 + 64)
#define BLOCK_SIZE   33024

#define STR2(x) #x
#define STR(x) STR2(x)

typedef struct {
    uint8_t bytes[BLOCK_SIZE];
} Block;

/* ldp of a pair of q or x registers of the state block */
#define LDPQ(a, b) "  ldp q" #a ", q" #b ", [x28, #(" STR(OFF_V) " + 16 * " #a ")]\n"
#define LDPX(a, b) "  ldp x" #a ", x" #b ", [x28, #(" STR(OFF_X) " + 8 * " #a ")]\n"

/*
 * The code chunks. They are assembled in this file and copied into the JIT region. x28 is set
 * by the entry chunk from the argument (x0); every other chunk is position independent.
 */
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".arch_extension sme\n"
    ".arch_extension sme2\n"
    ".arch_extension sve\n"
    ".arch_extension sve2\n"

    ".globl _chunk_entry\n_chunk_entry:\n"
    "  mov x28, x0\n"
    ".globl _chunk_entry_end\n_chunk_entry_end:\n"

    ".globl _chunk_smstart_sm\n_chunk_smstart_sm:\n"
    "  smstart sm\n"
    ".globl _chunk_smstart_sm_end\n_chunk_smstart_sm_end:\n"

    ".globl _chunk_smstart_za\n_chunk_smstart_za:\n"
    "  smstart za\n"
    ".globl _chunk_smstart_za_end\n_chunk_smstart_za_end:\n"

    /* z0-z31 and p0-p15 from the block (the registers of the streaming mode) */
    ".globl _chunk_load_z\n_chunk_load_z:\n"
    "  mov x0, #" STR(OFF_Z) "\n"
    "  add x0, x28, x0\n"
    "  .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
    "  ldr z\\n, [x0, #\\n, mul vl]\n"
    "  .endr\n"
    "  mov x0, #" STR(OFF_P) "\n"
    "  add x0, x28, x0\n"
    "  .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
    "  ldr p\\n, [x0, #\\n, mul vl]\n"
    "  .endr\n"
    ".globl _chunk_load_z_end\n_chunk_load_z_end:\n"

    /* the rows of za, zt0 */
    ".globl _chunk_load_za\n_chunk_load_za:\n"
    "  mov w12, #0\n"
    "  mov x0, #" STR(OFF_ZA) "\n"
    "  add x0, x28, x0\n"
    "  rdsvl x1, #1\n"
    "1:\n"
    "  ldr za[w12, 0], [x0]\n"
    "  addsvl x0, x0, #1\n"
    "  add w12, w12, #1\n"
    "  cmp w12, w1\n"
    "  b.lo 1b\n"
    "  mov x0, #" STR(OFF_ZT0) "\n"
    "  add x0, x28, x0\n"
    "  ldr zt0, [x0]\n"
    ".globl _chunk_load_za_end\n_chunk_load_za_end:\n"

    /* v0-v31 */
    ".globl _chunk_load_v\n_chunk_load_v:\n"
    LDPQ(0, 1) LDPQ(2, 3) LDPQ(4, 5) LDPQ(6, 7) LDPQ(8, 9) LDPQ(10, 11) LDPQ(12, 13)
    LDPQ(14, 15) LDPQ(16, 17) LDPQ(18, 19) LDPQ(20, 21) LDPQ(22, 23) LDPQ(24, 25)
    LDPQ(26, 27) LDPQ(28, 29) LDPQ(30, 31)
    ".globl _chunk_load_v_end\n_chunk_load_v_end:\n"

    /* fpcr, fpsr, nzcv, sp and the x registers; the last chunk of the prologue */
    ".globl _chunk_load_core\n_chunk_load_core:\n"
    "  ldr x0, [x28, #" STR(OFF_FPCR) "]\n"
    "  msr fpcr, x0\n"
    "  ldr x0, [x28, #" STR(OFF_FPSR) "]\n"
    "  msr fpsr, x0\n"
    "  ldr x0, [x28, #" STR(OFF_NZCV) "]\n"
    "  msr nzcv, x0\n"
    "  ldr x0, [x28, #" STR(OFF_SP) "]\n"
    "  mov sp, x0\n"
    LDPX(0, 1) LDPX(2, 3) LDPX(4, 5) LDPX(6, 7) LDPX(8, 9) LDPX(10, 11) LDPX(12, 13)
    LDPX(14, 15) LDPX(16, 17) LDPX(18, 19) LDPX(20, 21) LDPX(22, 23) LDPX(24, 25)
    LDPX(26, 27)
    "  ldp x29, x30, [x28, #(" STR(OFF_X) " + 8 * 29)]\n"
    ".globl _chunk_load_core_end\n_chunk_load_core_end:\n"

    /* The epilogue of the streaming mode: z, p, fpcr, fpsr to the block, the registers it
       uses are saved and restored (x0, x1, x2, x12 and nzcv) */
    ".globl _chunk_store_z\n_chunk_store_z:\n"
    "  stp x0, x1, [x28, #" STR(OFF_SAVE) "]\n"
    "  stp x2, x12, [x28, #(" STR(OFF_SAVE) " + 16)]\n"
    "  mrs x0, nzcv\n"
    "  str x0, [x28, #(" STR(OFF_SAVE) " + 32)]\n"
    "  mrs x0, fpcr\n"
    "  str x0, [x28, #" STR(OFF_FPCR_OUT) "]\n"
    "  mrs x0, fpsr\n"
    "  str x0, [x28, #" STR(OFF_FPSR_OUT) "]\n"
    "  mov x0, #" STR(OFF_ZOUT) "\n"
    "  add x0, x28, x0\n"
    "  .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31\n"
    "  str z\\n, [x0, #\\n, mul vl]\n"
    "  .endr\n"
    "  mov x0, #" STR(OFF_POUT) "\n"
    "  add x0, x28, x0\n"
    "  .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
    "  str p\\n, [x0, #\\n, mul vl]\n"
    "  .endr\n"
    ".globl _chunk_store_z_end\n_chunk_store_z_end:\n"

    ".globl _chunk_store_za\n_chunk_store_za:\n"
    "  mov w12, #0\n"
    "  mov x0, #" STR(OFF_ZAOUT) "\n"
    "  add x0, x28, x0\n"
    "  rdsvl x1, #1\n"
    "1:\n"
    "  str za[w12, 0], [x0]\n"
    "  addsvl x0, x0, #1\n"
    "  add w12, w12, #1\n"
    "  cmp w12, w1\n"
    "  b.lo 1b\n"
    "  mov x0, #" STR(OFF_ZT0OUT) "\n"
    "  add x0, x28, x0\n"
    "  str zt0, [x0]\n"
    ".globl _chunk_store_za_end\n_chunk_store_za_end:\n"

    ".globl _chunk_restore\n_chunk_restore:\n"
    "  ldr x0, [x28, #(" STR(OFF_SAVE) " + 32)]\n"
    "  msr nzcv, x0\n"
    "  ldp x2, x12, [x28, #(" STR(OFF_SAVE) " + 16)]\n"
    "  ldp x0, x1, [x28, #" STR(OFF_SAVE) "]\n"
    ".globl _chunk_restore_end\n_chunk_restore_end:\n"

    /* smstop, run from C after a streaming run */
    ".globl _host_smstop\n_host_smstop:\n"
    "  smstop\n"
    "  ret\n"
);

extern const uint8_t chunk_entry[], chunk_entry_end[];
extern const uint8_t chunk_smstart_sm[], chunk_smstart_sm_end[];
extern const uint8_t chunk_smstart_za[], chunk_smstart_za_end[];
extern const uint8_t chunk_load_z[], chunk_load_z_end[];
extern const uint8_t chunk_load_za[], chunk_load_za_end[];
extern const uint8_t chunk_load_v[], chunk_load_v_end[];
extern const uint8_t chunk_load_core[], chunk_load_core_end[];
extern const uint8_t chunk_store_z[], chunk_store_z_end[];
extern const uint8_t chunk_store_za[], chunk_store_za_end[];
extern const uint8_t chunk_restore[], chunk_restore_end[];
extern void host_smstop(void);

/* ------------------------------------------------------------------------------------------ */

typedef struct {
    int kind;            /* 0 none, 1 fall, 2 branch, 3 undef, 4 fault, 5 brk, 6 timeout */
    uint64_t pc;
    uint64_t x[31];
    uint64_t sp;
    uint32_t nzcv;
    uint32_t fpcr, fpsr;
    uint8_t v[32][16];
} Capture;

static Capture gCapture;
static sigjmp_buf gJump;
static uint64_t gTestLow, gTestHigh, gEndMarker;
static int gStreaming;        /* the run is in the streaming mode: fpcr / fpsr / v come from the block */
static Block* gBlock;

static int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse the hex digits of 'text' as a little endian number of 'size' bytes. Returns the digits */
static int parseWide(const char* text, uint8_t* bytes, int size)
{
    int digits = 0;
    while (hexValue(text[digits]) >= 0)
        digits++;
    memset(bytes, 0, size);
    for (int i = 0; i < digits; i++)
    {
        int nibble = hexValue(text[digits - 1 - i]);
        if (i / 2 < size)
            bytes[i / 2] |= (uint8_t)(nibble << ((i % 2) * 4));
    }
    return digits;
}

static uint64_t parse64(const char* text)
{
    uint8_t b[8];
    parseWide(text, b, 8);
    uint64_t v;
    memcpy(&v, b, 8);
    return v;
}

static void handler(int sig, siginfo_t* info, void* context)
{
    /* The signal arrives in the streaming mode of the interrupted code, where the Advanced SIMD
       instructions (memcpy of libc) are illegal: leave it first */
    __asm__ volatile(".arch_extension sme\n smstop" ::: "memory");
    ucontext_t* uc = (ucontext_t*)context;
    _STRUCT_ARM_THREAD_STATE64* ss = &uc->uc_mcontext->__ss;
    uint64_t pc = arm_thread_state64_get_pc(*ss);
    Capture* c = &gCapture;
    for (int i = 0; i < 29; i++)
        c->x[i] = ss->__x[i];
    c->x[29] = arm_thread_state64_get_fp(*ss);
    c->x[30] = (uint64_t)arm_thread_state64_get_lr(*ss);
    c->sp = arm_thread_state64_get_sp(*ss);
    c->nzcv = (ss->__cpsr >> 28) & 0xF;
    if (!gStreaming)
    {
        _STRUCT_ARM_NEON_STATE64* ns = &uc->uc_mcontext->__ns;
        memcpy(c->v, ns->__v, sizeof(c->v));
        c->fpcr = ns->__fpcr;
        c->fpsr = ns->__fpsr;
    }
    c->pc = pc;
    switch (sig)
    {
    case SIGTRAP:
        if (pc == gEndMarker)
            c->kind = 1;
        else if (pc >= gTestLow && pc < gTestHigh)
            c->kind = 5;
        else
            c->kind = 2;
        break;
    case SIGILL:
        c->kind = (pc >= gTestLow && pc < gTestHigh) ? 3 : 2;
        break;
    case SIGALRM:
        c->kind = 6;
        break;
    default:
        /* SIGSEGV, SIGBUS: inside the instruction it's a data access, elsewhere the fetch of
           the target of a branch */
        c->kind = (pc >= gTestLow && pc < gTestHigh) ? 4 : 2;
        break;
    }
    siglongjmp(gJump, 1);
}

static uint32_t encodeBrk(uint32_t imm)
{
    return 0xD4200000u | (imm << 5);
}

/* The memory window and its initial image */
static uint8_t* gWindow;
static uint8_t gPattern[MEM_SIZE];      /* the pattern of the window */
static uint8_t gInitial[MEM_SIZE];      /* the window at the start of a run: the pattern + the mem tokens */

static uint8_t patternByte(uint64_t a)
{
    return (uint8_t)(((uint32_t)a * 2654435761u) >> 24);
}

/* The code region */
static uint8_t* gRegion;
static uint64_t gRegionBase;
static size_t gRegionSize;
static size_t gPage;

static int mapRegion(uint64_t base, size_t size)
{
    if (gRegion != NULL && gRegionBase == base && gRegionSize == size)
        return 1;
    if (gRegion != NULL)
        munmap(gRegion, gRegionSize);
    gRegion = mmap((void*)base, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (gRegion == MAP_FAILED || (uint64_t)gRegion != base)
    {
        if (gRegion != MAP_FAILED)
            munmap(gRegion, size);
        gRegion = NULL;
        return 0;
    }
    gRegionBase = base;
    gRegionSize = size;
    return 1;
}

typedef struct {
    int sm, za;
    uint64_t pc;
    uint8_t zbytes[32][SVL_BYTES];
    int hasZ;
    uint8_t pbytes[16][8];
    uint8_t zarows[ZA_ROWS][SVL_BYTES];
    uint8_t zt0[64];
} Extra;

static void printVector(const uint8_t* bytes, int size)
{
    for (int b = size - 1; b >= 0; b--)
        printf("%02x", bytes[b]);
}

static void emitWords(uint8_t* region, size_t* pos, const uint8_t* from, const uint8_t* to)
{
    memcpy(region + *pos, from, (size_t)(to - from));
    *pos += (size_t)(to - from);
}

/* Run a line. Returns 0 if it printed the result, otherwise the error message */
static const char* runLine(char* line)
{
    char* semicolon = strchr(line, ';');
    if (semicolon == NULL)
        return "bad line";
    uint8_t code[256];
    size_t codeSize = 0;
    char* p = line;
    while (hexValue(p[0]) >= 0 && hexValue(p[1]) >= 0 && codeSize < sizeof(code))
    {
        code[codeSize++] = (uint8_t)(hexValue(p[0]) * 16 + hexValue(p[1]));
        p += 2;
    }
    if (codeSize == 0 || (codeSize % 4) != 0)
        return "bad code";
    for (size_t i = 0; i < codeSize; i += 4)
    {
        uint32_t word;
        memcpy(&word, code + i, 4);
        if ((word & 0xFFE0001Fu) == 0xD4000001u)
            return "svc is not run on the host";
    }

    Block* blk = gBlock;
    memset(blk, 0, sizeof(*blk));
    memset(&gCapture, 0, sizeof(gCapture));
    Extra* ex = calloc(1, sizeof(Extra));
    ex->pc = A64_PC_DEFAULT;
    uint64_t sp = MEM_BASE + MEM_SIZE / 2;
    uint64_t nzcv = 0, fpcr = 0, fpsr = 0;
    uint64_t svl = 512;
    int sve = 0;
    memcpy(gWindow, gPattern, MEM_SIZE);
    uint8_t v[32][16];
    memset(v, 0, sizeof(v));
    int hasV[32] = { 0 };

    for (char* tok = strtok(semicolon + 1, " \n\r"); tok != NULL; tok = strtok(NULL, " \n\r"))
    {
        char* eq = strchr(tok, '=');
        if (eq == NULL)
            continue;
        *eq = 0;
        const char* value = eq + 1;
        int n;
        if (tok[0] == 'x' && tok[1] >= '0' && tok[1] <= '9')
        {
            n = atoi(tok + 1);
            uint64_t val = parse64(value);
            if (n < 31)
                memcpy(blk->bytes + OFF_X + 8 * n, &val, 8);
        }
        else if (strcmp(tok, "sp") == 0)
            sp = parse64(value);
        else if (strcmp(tok, "pc") == 0)
            ex->pc = parse64(value);
        else if (strcmp(tok, "nzcv") == 0)
            nzcv = parse64(value);
        else if (strcmp(tok, "fpcr") == 0)
            fpcr = parse64(value);
        else if (strcmp(tok, "fpsr") == 0)
            fpsr = parse64(value);
        else if (strcmp(tok, "sve") == 0)
            sve = (int)parse64(value);
        else if (strcmp(tok, "sm") == 0)
            ex->sm = (int)parse64(value);
        else if (strcmp(tok, "za") == 0)
            ex->za = (int)parse64(value);
        else if (strcmp(tok, "vl") == 0)
            (void)parse64(value);
        else if (strcmp(tok, "svl") == 0)
            svl = parse64(value);
        else if (tok[0] == 'v' && tok[1] >= '0' && tok[1] <= '9')
        {
            n = atoi(tok + 1);
            parseWide(value, v[n], 16);
            hasV[n] = 1;
        }
        else if (tok[0] == 'z' && tok[1] >= '0' && tok[1] <= '9')
        {
            n = atoi(tok + 1);
            parseWide(value, ex->zbytes[n], SVL_BYTES);
            ex->hasZ = 1;
        }
        else if (tok[0] == 'p' && tok[1] >= '0' && tok[1] <= '9')
        {
            n = atoi(tok + 1);
            parseWide(value, ex->pbytes[n], 8);
        }
        else if (tok[0] == 'z' && tok[1] == 'a' && tok[2] >= '0' && tok[2] <= '9')
        {
            n = atoi(tok + 2);
            if (n < ZA_ROWS)
                parseWide(value, ex->zarows[n], SVL_BYTES);
        }
        else if (strcmp(tok, "zt0") == 0)
            parseWide(value, ex->zt0, 64);
        else if (strcmp(tok, "ffr") == 0)
            ;
        else if (strcmp(tok, "mem") == 0)
        {
            /* addr:hex,addr:hex */
            char* run = (char*)value;
            while (*run != 0 && *run != '-')
            {
                uint64_t addr = parse64(run);
                char* colon = strchr(run, ':');
                if (colon == NULL)
                    break;
                char* d = colon + 1;
                size_t k = 0;
                while (hexValue(d[0]) >= 0 && hexValue(d[1]) >= 0)
                {
                    uint64_t a = addr + k;
                    if (a < MEM_BASE || a >= MEM_BASE + MEM_SIZE)
                    {
                        free(ex);
                        return "mem outside of the window";
                    }
                    gWindow[a - MEM_BASE] = (uint8_t)(hexValue(d[0]) * 16 + hexValue(d[1]));
                    d += 2;
                    k++;
                }
                run = (*d == ',') ? d + 1 : d;
            }
        }
        else
        {
            free(ex);
            return "unknown token";
        }
    }
    if (sve && !ex->sm)
    {
        free(ex);
        return "the host has no SVE outside of the streaming mode";
    }
    if (ex->sm && svl != SVL_BYTES * 8)
    {
        free(ex);
        return "the streaming vector length of the host is 512 bits";
    }
    if (ex->za && !ex->sm && 0)
        ;

    memcpy(gInitial, gWindow, MEM_SIZE);

    /* The block */
    memcpy(blk->bytes + OFF_SP, &sp, 8);
    nzcv <<= 28;
    memcpy(blk->bytes + OFF_NZCV, &nzcv, 8);
    memcpy(blk->bytes + OFF_FPCR, &fpcr, 8);
    memcpy(blk->bytes + OFF_FPSR, &fpsr, 8);
    for (int i = 0; i < 32; i++)
        memcpy(blk->bytes + OFF_V + 16 * i, v[i], 16);
    if (ex->sm)
    {
        /* z with the low 128 bits from v where the state has one */
        for (int i = 0; i < 32; i++)
        {
            if (hasV[i])
                memcpy(ex->zbytes[i], v[i], 16);
            memcpy(blk->bytes + OFF_Z + SVL_BYTES * i, ex->zbytes[i], SVL_BYTES);
        }
        for (int i = 0; i < 16; i++)
            memcpy(blk->bytes + OFF_P + 8 * i, ex->pbytes[i], 8);
    }
    if (ex->za)
    {
        /* (za and zt0 are loaded outside of the streaming mode too: ldr za, ldr zt0, zero run) */
        for (int i = 0; i < ZA_ROWS; i++)
            memcpy(blk->bytes + OFF_ZA + SVL_BYTES * i, ex->zarows[i], SVL_BYTES);
        memcpy(blk->bytes + OFF_ZT0, ex->zt0, 64);
    }
    gStreaming = ex->sm;

    /* The region: [prologue page][the page of the pc][a page] */
    uint64_t pageBase = ex->pc & ~(uint64_t)(gPage - 1);
    uint64_t regionBase = pageBase - gPage;
    size_t regionSize = 3 * gPage;
    if (!mapRegion(regionBase, regionSize))
    {
        free(ex);
        return "cannot map the code at the pc of the state";
    }
    size_t prologueMax = gPage;
    uint8_t* region = gRegion;
    pthread_jit_write_protect_np(0);
    uint32_t sled = encodeBrk(SLED_IMM);
    for (size_t i = 0; i < regionSize; i += 4)
        memcpy(region + i, &sled, 4);

    /* The prologue */
    size_t pos = 0;
    emitWords(region, &pos, chunk_entry, chunk_entry_end);
    if (ex->sm)
        emitWords(region, &pos, chunk_smstart_sm, chunk_smstart_sm_end);
    if (ex->za)
        emitWords(region, &pos, chunk_smstart_za, chunk_smstart_za_end);
    if (ex->sm)
        emitWords(region, &pos, chunk_load_z, chunk_load_z_end);
    else
        emitWords(region, &pos, chunk_load_v, chunk_load_v_end);
    if (ex->za)
        emitWords(region, &pos, chunk_load_za, chunk_load_za_end);
    emitWords(region, &pos, chunk_load_core, chunk_load_core_end);
    /* b to the code */
    {
        int64_t distance = ((int64_t)ex->pc - (int64_t)(regionBase + pos)) / 4;
        uint32_t branch = 0x14000000u | ((uint32_t)distance & 0x03FFFFFFu);
        memcpy(region + pos, &branch, 4);
        pos += 4;
    }
    if (pos > prologueMax)
    {
        pthread_jit_write_protect_np(1);
        free(ex);
        return "prologue too large";
    }

    /* The code and the epilogue */
    size_t codeOffset = (size_t)(ex->pc - regionBase);
    memcpy(region + codeOffset, code, codeSize);
    size_t epi = codeOffset + codeSize;
    if (ex->sm)
    {
        emitWords(region, &epi, chunk_store_z, chunk_store_z_end);
        if (ex->za)
            emitWords(region, &epi, chunk_store_za, chunk_store_za_end);
        emitWords(region, &epi, chunk_restore, chunk_restore_end);
    }
    uint32_t end = encodeBrk(END_IMM);
    memcpy(region + epi, &end, 4);
    gTestLow = ex->pc;
    gTestHigh = ex->pc + codeSize;
    gEndMarker = regionBase + epi;
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(region, regionSize);

    /* Run */
    struct itimerval timer = { { 0, 0 }, { 2, 0 } };
    setitimer(ITIMER_REAL, &timer, NULL);
    if (sigsetjmp(gJump, 1) == 0)
    {
        ((void (*)(Block*))region)(blk);
        /* never returns */
    }
    struct itimerval off = { { 0, 0 }, { 0, 0 } };
    setitimer(ITIMER_REAL, &off, NULL);
    /* back from the handler: restore the state of the process */
    __asm__ volatile("msr fpcr, xzr\n msr fpsr, xzr" ::: "memory");
    Capture* c = &gCapture;
    if (c->kind == 6)
    {
        free(ex);
        return "timeout";
    }

    /* The result */
    static const char* const exits[] = { "none", "fall", "branch", "trap:undef", "trap:fault",
                                         "trap:brk" };
    if (c->kind == 1)
        c->pc = ex->pc + codeSize;
    if (ex->sm)
    {
        /* v is the low 128 bits of z, fpcr and fpsr come from the epilogue */
        c->fpcr = (uint32_t)*(uint64_t*)(blk->bytes + OFF_FPCR_OUT);
        c->fpsr = (uint32_t)*(uint64_t*)(blk->bytes + OFF_FPSR_OUT);
        for (int i = 0; i < 32; i++)
            memcpy(c->v[i], blk->bytes + OFF_ZOUT + SVL_BYTES * i, 16);
    }
    printf("OK exit=%s", exits[c->kind]);
    if (ex->sm && c->kind != 1)
        printf(" unknown=fpcr,fpsr");
    if (ex->sm)
        printf(" sve=1 sm=1");
    if (ex->za)
        printf(" za=1");
    if (ex->sm)
        printf(" vl=%x svl=%x", (unsigned)(SVL_BYTES * 8), (unsigned)(SVL_BYTES * 8));
    for (int i = 0; i < 31; i++)
        printf(" x%d=%016llx", i, (unsigned long long)c->x[i]);
    printf(" sp=%016llx pc=%016llx nzcv=%x fpcr=%08x fpsr=%08x", (unsigned long long)c->sp,
           (unsigned long long)c->pc, c->nzcv, c->fpcr, c->fpsr);
    for (int i = 0; i < 32; i++)
    {
        printf(" v%d=", i);
        printVector(c->v[i], 16);
    }
    if (ex->sm && c->kind == 1)
    {
        for (int i = 0; i < 32; i++)
        {
            printf(" z%d=", i);
            printVector(blk->bytes + OFF_ZOUT + SVL_BYTES * i, SVL_BYTES);
        }
        for (int i = 0; i < 16; i++)
        {
            printf(" p%d=", i);
            printVector(blk->bytes + OFF_POUT + 8 * i, 8);
        }
        if (ex->za)
        {
            for (int i = 0; i < ZA_ROWS; i++)
            {
                printf(" za%d=", i);
                printVector(blk->bytes + OFF_ZAOUT + SVL_BYTES * i, SVL_BYTES);
            }
            printf(" zt0=");
            printVector(blk->bytes + OFF_ZT0OUT, 64);
        }
    }
    /* The memory: the changed bytes, in runs */
    printf(" mem=");
    int first = 1;
    for (int i = 0; i < MEM_SIZE;)
    {
        if (gWindow[i] == gInitial[i])
        {
            i++;
            continue;
        }
        int j = i;
        while (j < MEM_SIZE && gWindow[j] != gInitial[j])
            j++;
        printf("%s%llx:", first ? "" : ",", (unsigned long long)(MEM_BASE + i));
        for (int k = i; k < j; k++)
            printf("%02x", gWindow[k]);
        first = 0;
        i = j;
    }
    if (first)
        printf("-");
    printf("\n");
    free(ex);
    return NULL;
}

int main(int argc, char** argv)
{
    gPage = (size_t)getpagesize();
    if (argc > 1 && strcmp(argv[1], "--info") == 0)
    {
        printf("page=%zu svl_bytes=%d\n", gPage, SVL_BYTES);
        return 0;
    }
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    gWindow = mmap((void*)MEM_BASE, MEM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (gWindow == MAP_FAILED || (uint64_t)gWindow != MEM_BASE)
    {
        fprintf(stderr, "cannot map the memory window at %llx\n", (unsigned long long)MEM_BASE);
        return 1;
    }
    for (int i = 0; i < MEM_SIZE; i++)
        gPattern[i] = patternByte(MEM_BASE + i);
    gBlock = aligned_alloc(64, (BLOCK_SIZE + 63) & ~63);
    stack_t alt;
    alt.ss_sp = malloc(1 << 16);
    alt.ss_size = 1 << 16;
    alt.ss_flags = 0;
    sigaltstack(&alt, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    int sigs[] = { SIGILL, SIGTRAP, SIGSEGV, SIGBUS, SIGALRM, SIGFPE };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);

    static char line[1 << 20];
    while (fgets(line, sizeof(line), stdin))
    {
        const char* err = runLine(line);
        if (err != NULL)
            printf("ERR %s\n", err);
        fflush(stdout);
    }
    return 0;
}
