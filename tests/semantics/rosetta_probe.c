/*
 * rosetta_probe.c
 *
 * A second oracle for the register-only SSE / SSE2 instructions of the
 * semantics tests: it executes one instruction on the host's x86 floating
 * point unit (Rosetta 2 on an Apple Silicon Mac, or any x86-64 host) from a
 * given xmm / MXCSR / general register state and prints the result,
 * including the MXCSR exception flags, which QEMU 5 (Unicorn) never reports.
 *
 *   clang -arch x86_64 -O1 -o out/rosetta_probe tests/semantics/rosetta_probe.c
 *
 * Input, one instruction per line, the protocol of `dismount_cli run`:
 *   <hex bytes of the instruction> ; eax=.. ecx=.. xmm0=<32 hex digits> ... mxcsr=..
 * Output, one line for each input line:
 *   OK eax=.. ecx=.. edx=.. ebx=.. ebp=.. esi=.. edi=.. eflags=.. mxcsr=.. xmm0=.. ... xmm7=..
 * (eflags: only CF PF AF ZF SF OF).
 *
 * The instruction must not touch memory or esp: the caller checks that.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>

/* The state block (r15 points to it while the instruction runs) */
enum {
    OFF_XMM_IN = 0,        /* 8 * 16 bytes */
    OFF_MXCSR_IN = 128,
    OFF_MXCSR_OUT = 132,
    OFF_FLAGS = 136,
    OFF_GPR_IN = 144,      /* rax rcx rdx rbx rbp rsi rdi: 8 bytes each */
    OFF_GPR_OUT = 208,
    OFF_XMM_OUT = 272,     /* 8 * 16 bytes */
    STATE_SIZE = 400
};

static const char* const GPR_NAMES[7] = { "eax", "ecx", "edx", "ebx", "ebp", "esi", "edi" };
/* The ModRM numbers of the registers rax rcx rdx rbx rbp rsi rdi */
static const int GPR_NUMBERS[7] = { 0, 1, 2, 3, 5, 6, 7 };

static uint8_t* gStub;
static size_t gStubSize;

static void emit(const uint8_t* bytes, size_t count)
{
    memcpy(gStub + gStubSize, bytes, count);
    gStubSize += count;
}

/* REX.B 0F op modrm(mod=2, reg, rm=7 -> r15) disp32 */
static void emitSse(uint8_t opcode, int reg, int disp)
{
    uint8_t b[8] = { 0x41, 0x0F, opcode, (uint8_t)(0x87 | (reg << 3)), 0, 0, 0, 0 };
    memcpy(b + 4, &disp, 4);
    emit(b, 8);
}

/* REX.W+B op modrm(mod=2, reg, rm=7) disp32: mov reg, [r15+disp] / mov [r15+disp], reg */
static void emitMov(uint8_t opcode, int reg, int disp)
{
    uint8_t b[8] = { 0x49, opcode, (uint8_t)(0x87 | (reg << 3)), 0, 0, 0, 0 };
    memcpy(b + 3, &disp, 4);
    emit(b, 7);
}

static void emitMxcsr(int extension, int disp)
{
    uint8_t b[8] = { 0x41, 0x0F, 0xAE, (uint8_t)(0x87 | (extension << 3)), 0, 0, 0, 0 };
    memcpy(b + 4, &disp, 4);
    emit(b, 8);
}

static int hexValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Parse hex digits of 'text' (up to a space) into little endian bytes */
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

int main(void)
{
    gStub = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANON, -1, 0);
    if (gStub == MAP_FAILED)
    {
        fprintf(stderr, "mmap failed\n");
        return 1;
    }
    char line[4096];
    uint8_t* state = calloc(1, STATE_SIZE);
    while (fgets(line, sizeof(line), stdin))
    {
        memset(state, 0, STATE_SIZE);
        uint32_t mxcsr = 0x1f80;
        uint32_t eflagsIn = 2;
        memcpy(state + OFF_MXCSR_IN, &mxcsr, 4);

        /* The instruction */
        uint8_t code[32];
        size_t codeSize = 0;
        char* p = line;
        while (hexValue(p[0]) >= 0 && hexValue(p[1]) >= 0 && codeSize < sizeof(code))
        {
            code[codeSize++] = (uint8_t)(hexValue(p[0]) * 16 + hexValue(p[1]));
            p += 2;
        }
        char* semicolon = strchr(line, ';');
        if (semicolon == NULL || codeSize == 0)
        {
            printf("ERR bad line\n");
            fflush(stdout);
            continue;
        }

        /* The state tokens */
        for (char* tok = strtok(semicolon + 1, " \n\r"); tok != NULL; tok = strtok(NULL, " \n\r"))
        {
            char* eq = strchr(tok, '=');
            if (eq == NULL)
                continue;
            *eq = 0;
            const char* value = eq + 1;
            if (strncmp(tok, "xmm", 3) == 0 && tok[3] >= '0' && tok[3] <= '7' && tok[4] == 0)
                parseWide(value, state + OFF_XMM_IN + 16 * (tok[3] - '0'), 16);
            else if (strcmp(tok, "mxcsr") == 0)
                parseWide(value, state + OFF_MXCSR_IN, 4);
            else if (strcmp(tok, "eflags") == 0)
                parseWide(value, (uint8_t*)&eflagsIn, 4);
            else
                for (int i = 0; i < 7; i++)
                    if (strcmp(tok, GPR_NAMES[i]) == 0)
                        parseWide(value, state + OFF_GPR_IN + 8 * i, 4);
        }

        /* The stub: prologue, load the state, the instruction, store it */
        gStubSize = 0;
        {
            const uint8_t prologue[] = { 0x53, 0x55, 0x41, 0x57, 0x49, 0x89, 0xFF };
            emit(prologue, sizeof(prologue));          /* push rbx; push rbp; push r15; mov r15, rdi */
        }
        for (int i = 0; i < 8; i++)
            emitSse(0x10, i, OFF_XMM_IN + 16 * i);     /* movups xmm_i, [r15+...] */
        emitMxcsr(2, OFF_MXCSR_IN);                    /* ldmxcsr */
        for (int i = 0; i < 7; i++)
            emitMov(0x8B, GPR_NUMBERS[i], OFF_GPR_IN + 8 * i);
        {
            /* push imm32; popfq: CF PF AF ZF SF OF of the input */
            uint32_t flagsValue = (eflagsIn & 0x8D5) | 2;
            uint8_t setFlags[6] = { 0x68, 0, 0, 0, 0, 0x9D };
            memcpy(setFlags + 1, &flagsValue, 4);
            emit(setFlags, sizeof(setFlags));
        }
        emit(code, codeSize);
        for (int i = 0; i < 7; i++)
            emitMov(0x89, GPR_NUMBERS[i], OFF_GPR_OUT + 8 * i);
        {
            const uint8_t flags[] = { 0x9C, 0x58 };               /* pushfq; pop rax */
            emit(flags, sizeof(flags));
        }
        emitMov(0x89, 0, OFF_FLAGS);
        emitMxcsr(3, OFF_MXCSR_OUT);                   /* stmxcsr */
        for (int i = 0; i < 8; i++)
            emitSse(0x11, i, OFF_XMM_OUT + 16 * i);    /* movups [r15+...], xmm_i */
        {
            const uint8_t epilogue[] = { 0xFC, 0x41, 0x5F, 0x5D, 0x5B, 0xC3 };
            emit(epilogue, sizeof(epilogue));          /* cld; pop r15; pop rbp; pop rbx; ret */
        }

        ((void (*)(uint8_t*))gStub)(state);

        uint64_t flags;
        uint32_t mxcsrOut;
        memcpy(&flags, state + OFF_FLAGS, 8);
        memcpy(&mxcsrOut, state + OFF_MXCSR_OUT, 4);
        printf("OK");
        for (int i = 0; i < 7; i++)
        {
            uint32_t value;
            memcpy(&value, state + OFF_GPR_OUT + 8 * i, 4);
            printf(" %s=%08x", GPR_NAMES[i], value);
        }
        printf(" eflags=%08x mxcsr=%08x", (uint32_t)(flags & 0x8D5), mxcsrOut);
        for (int i = 0; i < 8; i++)
        {
            printf(" xmm%d=", i);
            for (int b = 15; b >= 0; b--)
                printf("%02x", state[OFF_XMM_OUT + 16 * i + b]);
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}
