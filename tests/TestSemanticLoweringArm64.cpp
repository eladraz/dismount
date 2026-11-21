/*
 * TestSemanticLoweringArm64.cpp
 *
 * The differential test of the A64 lowering (lift, interpret,
 * legalize, lower, run). Each row of the A64 corpus is one instruction, or a few of them, of
 * the mnemonic set of the Lua 5.4.7 guest (the 109 mnemonics of
 * its -O2 build). For each row:
 *
 *   1. the words are decoded and lifted for the AArch64 guest
 *      (Arm64Architecture(128, 128));
 *   2. the lifted block is run by the SemanticInterpreter, unlegalized;
 *   3. a copy is legalized for AMD64 (SemanticLegalizer), lowered with
 *      AMD64Lowering::lowerBlock() onto a state layout that mirrors the one
 *      of reforge (x0-x30 and sp at 8 bytes, the flags at 8 bytes, z0-z31 and
 *      their views at 16 bytes, the temporaries at 16 bytes), linked into
 *      executable memory with a stub for each guest label, and run natively;
 *   4. the state the two runs leave is compared: x0-x30, sp, N, Z, C, V, the
 *      low 128 bits of v0-v31, the guest memory window, and the exit (a
 *      branch, call or return to a guest address; a trap with its kind and
 *      number; or a fall through to a label). A scalar floating point result
 *      compares its NaN by class: the sign and payload of a NaN are the
 *      documented deviation of AMD64LoweringFloat.cpp.
 *
 * The inputs of a row are the setup of the corpus (when it has one) and
 * random inputs with edge values (zeros, the extremes, the NaNs and the
 * infinities). The memory rows keep their addresses in the guest memory
 * window (0x4000-0x4FFF).
 *
 * The skipped rows, each with its reason in the output:
 *   - half precision (h registers): the legalizer and the lowering refuse the
 *     16-bit float operations; the Lua guest uses none (the float lowering
 *     covers single and double);
 *   - a literal load (ldr x0, #16): it reads the code, outside the window;
 *   - a corpus row which expects a guest memory fault: the lowered code has
 *     no bounds check (the faults are the runtime's);
 *   - no executable memory on the host.
 *
 * Floating point: the lowering rounds to nearest, flushes nothing and keeps
 * the default NaN off, which is FPCR = 0. That is the state of the Lua guest
 * and of the runtime, so every case runs with FPCR = 0 (the setup's FPCR is
 * ignored), and a non-default FPCR is out of scope. FPSR and FPCR are not
 * compared: the cumulative flags of FPSR are not kept by the lowering. A NaN
 * result compares by class (its sign and payload are the documented deviation).
 *
 * The exit of each mnemonic is checked against the lowering contract: a
 * branch, a conditional branch, a compare-and-branch or a test-and-branch to
 * a constant address, and bl, are plain jumps to a label; br exits with the
 * branch reason, blr with the call reason, ret with the return reason; brk
 * exits with the trap reason (kind and number compared).
 *
 * Output: "arm64 rows: <N> run, <F> failed, <K> skipped", then one coverage
 * line per mnemonic (rows passed and skipped). Every mnemonic must have at
 * least one passing row, and every skipped row prints its reason.
 *
 * The native runs need an executable mapping and an x86-64 Linux host; the
 * other hosts only print that the rows did not run.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <stdint.h>
#define LOWERING_ARM64_NATIVE 1
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/smartptr.h"
#include "xStl/data/sarray.h"
#include "xStl/data/list.h"
#include "xStl/exceptions.h"
#include "xStl/stream/ioStream.h"
#include "xStl/data/datastream.h"
#include "xStl/stream/memoryStream.h"
#include "xStl/../../tests/tests.h"
#include "dismount/OpcodeSubsystems.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/SemanticInstruction.h"
#include "dismount/semantics/SemanticOperand.h"
#include "dismount/semantics/SemanticOperation.h"
#include "dismount/semantics/SemanticType.h"
#include "dismount/semantics/ArchitectureDescription.h"
#include "dismount/semantics/SemanticLowering.h"
#include "dismount/semantics/SemanticLegalizer.h"
#include "dismount/semantics/SemanticInterpreter.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/assembler/StackInterface.h"
#include "dismount/assembler/AssemblingFactory.h"
#include "dismount/assembler/proc/ia32/IA32Assembler.h"
#include "dismount/proc/ia32/AMD64Architecture.h"
#include "dismount/proc/ia32/AMD64Lowering.h"
#include "dismount/proc/arm/ArmInstructionSet.h"
#include "dismount/proc/arm/ArmFeatures.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/proc/arm/Arm64Architecture.h"
#include "dismount/proc/arm/ArmSemantics.h"

#ifdef LOWERING_ARM64_NATIVE
#include <sys/mman.h>
#endif

// The rows of the A64 corpus. Each is { {words}, count, text, setup, expected }
// (the expected state is not used: the oracle is the interpreter)
struct CorpusRow {
    uint32 m_words[4];
    uint m_count;
    const char* m_text;
    const char* m_setup;
    const char* m_expected;
};
#include "arm/Arm64SemanticRows.inc"
#include "arm/Arm64SimdSemanticRowsFloat.inc"
#include "arm/Arm64SimdSemanticRowsInteger.inc"

// The disassembler rows, { address, word, text } and { address, word, text,
// features }: they have no setup, and supply the mnemonics the corpus of the
// semantics doesn't have in scalar form
struct TextRow3 {
    uint64 m_address;
    uint32 m_word;
    const char* m_text;
};
struct TextRow4 {
    uint64 m_address;
    uint32 m_word;
    const char* m_text;
    const char* m_features;
};
#include "arm/Arm64TestRows.inc"
#include "arm/Arm64SimdTestRows.inc"

namespace {

// The mnemonics of the Lua 5.4.7 guest, 109 in all
const char* const MNEMONICS[] = {
    // Data movement
    "mov", "ldr", "str", "ldrb", "strb", "ldp", "stp", "adrp", "movk", "ldrh",
    "ldrsw", "ldur", "ldurb", "strh", "stur", "ldrsb", "sturb", "ldrsh",
    "ldursb", "sturh", "ldpsw",
    // Integer ALU
    "add", "cmp", "sub", "and", "orr", "lsr", "tst", "csel", "lsl", "subs",
    "cmn", "cset", "mvn", "eor", "smaddl", "sxtw", "neg", "ubfx", "udiv",
    "ccmp", "bfi", "umaddl", "msub", "madd", "csinc", "bic", "mul", "asr",
    "cinc", "ubfiz", "ands", "umulh", "sbfiz", "sdiv", "ccmn", "adds", "ror",
    "csinv", "sxtb", "smull", "sbfx", "bfxil", "smsubl", "negs", "mneg",
    "csneg", "cneg", "umull", "sxth",
    // The static musl guests: carry chains, exclusives, acquire/release,
    // address formation, vector permute and the remaining scalar forms
    "adcs", "adr", "bics", "dup", "ext", "ldaxr", "stlxr", "ldxr", "stxr",
    "ldar", "stlr", "ldurh", "ldursw", "umsubl",
    // Branches
    "bl", "b", "b.ne", "b.eq", "cbz", "b.lo", "b.lt", "ret", "cbnz", "b.hs",
    "b.hi", "b.gt", "b.ge", "tbz", "blr", "tbnz", "b.ls", "b.le", "br", "b.pl",
    "b.mi", "b.vs",
    // Traps and system registers
    "brk", "mrs", "msr", "dc",
    // Scalar floating point
    "fmov", "fcmp", "scvtf", "fcvtzs", "fcsel", "fadd", "fccmp", "fmul",
    "fdiv", "fsub", "fneg", "fcvt", "ucvtf",
    // Round to an integral value, and the float to integer conversions in the
    // rounding modes other than toward zero
    "frintm", "frintp", "frintz", "frinta", "frintx", "frinti", "frintn",
    "fcvtas", "fcvtau", "fcvtns", "fcvtnu", "fcvtms", "fcvtmu", "fcvtps", "fcvtpu",
    // The IEEE minimumNumber / maximumNumber of the ARM FMINNM and FMAXNM
    // (a signaling NaN gives the quieted NaN, FPCR.DN = 0)
    "fmaxnm", "fminnm",
    // The fused multiply-add and its negated forms (FMA3 on the host)
    "fmadd", "fmsub", "fnmadd", "fnmsub",
    // SIMD
    "cnt", "addv", "movi",
    // The LSE atomic memory operations of 8, 16, 32 and 64 bits (the
    // compare-and-swap, the swap and the load-and-operate forms with every
    // a/l/al ordering), lowered by AMD64LoweringAtomic.cpp. The pair forms
    // (casp, ldclrp, ldsetp, swpp) are not in the set: their 128 bit form is
    // refused by the lowering (no rbx in the register contract)
    "cas", "casalh", "casb", "ldadd", "ldaddal", "ldaddalb", "ldaddalh",
    "ldaddb", "ldaddh", "ldclr", "ldclral", "ldclralb", "ldclralh", "ldclrb",
    "ldclrh", "ldeor", "ldeoral", "ldeoralb", "ldeoralh", "ldeorb", "ldeorh",
    "ldset", "ldsetal", "ldsetalb", "ldsetalh", "ldsetb", "ldseth", "ldsmax",
    "ldsmaxal", "ldsmaxalb", "ldsmaxalh", "ldsmaxb", "ldsmaxh", "ldsmin",
    "ldsminal", "ldsminalb", "ldsminalh", "ldsminb", "ldsminh", "ldumax",
    "ldumaxal", "ldumaxalb", "ldumaxalh", "ldumaxb", "ldumaxh", "ldumin",
    "lduminal", "lduminalb", "lduminalh", "lduminb", "lduminh", "swp",
    "swpalb"
};
const uint MNEMONIC_COUNT = sizeof(MNEMONICS) / sizeof(MNEMONICS[0]);

// The fused multiply-adds of double and single precision, with
// the ARM NaN rule (the first signaling NaN of (a, n, m), else the first NaN;
// the default NaN for an invalid product whose addend is not signaling)
#define ARM_FUSED_ROWS \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=7ff0000000000001 v2=3ff0000000000000 v3=7ff8000000000002", "" }, \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=7ff8000000000001 v2=3ff0000000000000 v3=7ff0000000000001", "" }, \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=7ff8000000000001 v2=7ff0000000000002 v3=3ff0000000000000", "" }, \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=0000000000000000 v2=7ff0000000000000 v3=7ff8000000000005", "" }, \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=0000000000000000 v2=7ff0000000000000 v3=7ff0000000000001", "" }, \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=7ff0000000000000 v2=3ff0000000000000 v3=fff0000000000000", "" }, \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=fff8000000000001 v2=0000000000000000 v3=3ff0000000000000", "" }, \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=0000000000000000 v2=0000000000000000 v3=7ff8000000000003", "" }, \
{ {0x1F420C20}, 1, "fmadd d0, d1, d2, d3", "v1=3ff0000000000001 v2=3fefffffffffffff v3=bff0000000000000", "" }, \
{ {0x1F428C20}, 1, "fmsub d0, d1, d2, d3", "v1=7ff8000000000001 v2=3ff0000000000000 v3=7ff0000000000001", "" }, \
{ {0x1F428C20}, 1, "fmsub d0, d1, d2, d3", "v1=0000000000000000 v2=7ff0000000000000 v3=7ff8000000000005", "" }, \
{ {0x1F620C20}, 1, "fnmadd d0, d1, d2, d3", "v1=7ff8000000000001 v2=3ff0000000000000 v3=7ff0000000000001", "" }, \
{ {0x1F628C20}, 1, "fnmsub d0, d1, d2, d3", "v1=0000000000000000 v2=7ff0000000000000 v3=7ff0000000000001", "" }, \
{ {0x1F020C20}, 1, "fmadd s0, s1, s2, s3", "v1=7f800001 v2=3f800000 v3=7fc00002", "" }, \
{ {0x1F020C20}, 1, "fmadd s0, s1, s2, s3", "v1=7fc00001 v2=3f800000 v3=7f800001", "" }, \
{ {0x1F020C20}, 1, "fmadd s0, s1, s2, s3", "v1=7fc00001 v2=7f800002 v3=3f800000", "" }, \
{ {0x1F020C20}, 1, "fmadd s0, s1, s2, s3", "v1=00000000 v2=7f800000 v3=7fc00005", "" }, \
{ {0x1F020C20}, 1, "fmadd s0, s1, s2, s3", "v1=00000000 v2=7f800000 v3=7f800001", "" }, \
{ {0x1F020C20}, 1, "fmadd s0, s1, s2, s3", "v1=7f800000 v2=3f800000 v3=ff800000", "" }, \
{ {0x1F020C20}, 1, "fmadd s0, s1, s2, s3", "v1=ffc00001 v2=00000000 v3=3f800000", "" }, \
{ {0x1F020C20}, 1, "fmadd s0, s1, s2, s3", "v1=3f800001 v2=3f7fffff v3=bf800000", "" }, \
{ {0x1F028C20}, 1, "fmsub s0, s1, s2, s3", "v1=7fc00001 v2=7f800002 v3=3f800000", "" }, \
{ {0x1F220C20}, 1, "fnmadd s0, s1, s2, s3", "v1=00000000 v2=7f800000 v3=7f800001", "" }, \
{ {0x1F228C20}, 1, "fnmsub s0, s1, s2, s3", "v1=ffc00001 v2=00000000 v3=3f800000", "" }

// The rows of the static musl guests, { {words}, count, "text", "setup", "expected" }
// (expected is not used: the oracle is the interpreter)
#define PHASE4_ROWS \
    { {0xd53bd040}, 1, "mrs x0, tpidr_el0", "", "" }, \
    { {0xd53b00e5}, 1, "mrs x5, dczid_el0", "", "" }, \
    { {0xd51bd040}, 1, "msr tpidr_el0, x0", "x0=feedface", "" }, \
    { {0xd51bd040, 0xd53bd041}, 2, "msr tpidr_el0, x0 ; mrs x1, tpidr_el0", "x0=feedfacecafe", "" }, \
    { {0x8802fc23}, 1, "stlxr w2, w3, [x1]", "x1=4000 x3=1234567890 mem=4000:1011121314151617", "" }

#define ARM_NAN_ROWS \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=7ff0000000000001 v2=3ff0000000000000", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=3ff0000000000000 v2=7ff0000000000001", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=7ff8000000000001 v2=7ff0000000000001", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=7ff0000000000001 v2=7ff8000000000002", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=7ff8000000000001 v2=7ff8000000000002", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=fff0000000000001 v2=0000000000000000", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=0000000000000000 v2=8000000000000000", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=8000000000000000 v2=0000000000000000", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=7ff0000000000001 v2=7ff0000000000002", "" }, \
    { {0x1e626820}, 1, "fmaxnm d0, d1, d2", "v1=7ff8000000000001 v2=3ff0000000000000", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=7f800001 v2=3f800000", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=3f800000 v2=7f800001", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=7fc00001 v2=7f800001", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=7f800001 v2=7fc00002", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=7fc00001 v2=7fc00002", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=ff800001 v2=00000000", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=00000000 v2=80000000", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=80000000 v2=00000000", "" }, \
    { {0x1e226820}, 1, "fmaxnm s0, s1, s2", "v1=7f800001 v2=7f800002", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=7ff0000000000001 v2=3ff0000000000000", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=3ff0000000000000 v2=7ff0000000000001", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=7ff8000000000001 v2=7ff0000000000001", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=7ff0000000000001 v2=7ff8000000000002", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=7ff8000000000001 v2=7ff8000000000002", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=fff0000000000001 v2=0000000000000000", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=0000000000000000 v2=8000000000000000", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=8000000000000000 v2=0000000000000000", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=7ff0000000000001 v2=7ff0000000000002", "" }, \
    { {0x1e627820}, 1, "fminnm d0, d1, d2", "v1=7ff8000000000001 v2=3ff0000000000000", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=7f800001 v2=3f800000", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=3f800000 v2=7f800001", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=7fc00001 v2=7f800001", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=7f800001 v2=7fc00002", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=7fc00001 v2=7fc00002", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=ff800001 v2=00000000", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=00000000 v2=80000000", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=80000000 v2=00000000", "" }, \
    { {0x1e227820}, 1, "fminnm s0, s1, s2", "v1=7f800001 v2=7f800002", "" }


// The mnemonics which only the disassembler rows supply (no scalar row in the
// semantics corpus), and the simd rows (fcsel and fccmp)
const char* const TEXT_MNEMONICS[] = {
    "and", "mvn", "eor", "bic", "ldurb", "stur", "sturb", "ldursb", "sturh",
    "smull", "adcs", "adr", "bics", "ldaxr", "stlxr", "ldxr", "stxr", "ldar",
    "stlr", "ldurh", "ldursw", "umsubl", "mrs", "msr"
};
const char* const TEXT4_MNEMONICS[] = { "fcsel", "fccmp" };
// The integer SIMD rows the test takes (their other mnemonics are vector forms
// of the set above, and their scalar forms are not in the set)
const char* const SIMD_INTEGER_MNEMONICS[] = { "addv", "movi", "cnt", "dup", "ext" };
const uint SIMD_INTEGER_COUNT = sizeof(SIMD_INTEGER_MNEMONICS) /
                                sizeof(SIMD_INTEGER_MNEMONICS[0]);

// The guest address of the rows, and the window of the guest memory (the
// addresses the memory rows use). The memory of the host is the window's copy
const uint64 ROW_ADDRESS = 0x10000ULL;
const uint64 WINDOW_BASE = 0x4000ULL;
const uint WINDOW_SIZE = 0x1000;
// The base register the memory rows get when the corpus gives no value
const uint64 WINDOW_DEFAULT_BASE = 0x4800ULL;

// The number of cases of a row: the setup (or zeros) and the random inputs
const uint CASES_PER_ROW = 8;

// The label stubs of the native code store this reason (not an exit reason of
// the lowering, which are 1-4)
const uint64 REASON_LABEL = 0x1000ULL;

// The position of a mnemonic in MNEMONICS, or -1
int mnemonicIndex(const char* name, uint length)
{
    for (uint i = 0; i < MNEMONIC_COUNT; i++)
    {
        if ((strlen(MNEMONICS[i]) == length) &&
            (strncmp(MNEMONICS[i], name, length) == 0))
            return (int)i;
    }
    return -1;
}

// The mnemonic of a text (the characters up to the first space)
uint mnemonicLength(const char* text)
{
    uint length = 0;
    while ((text[length] != 0) && (text[length] != ' '))
        length++;
    return length;
}

// A vector text has an arrangement (.8b, .4s ...): a dot followed by a digit
bool isVectorText(const char* text)
{
    for (const char* p = text; *p != 0; p++)
    {
        if ((*p == '.') && (p[1] >= '0') && (p[1] <= '9'))
            return true;
    }
    return false;
}

// The text of the rows of an optional mnemonic set
bool inSet(const char* name, uint length, const char* const* set, uint count)
{
    for (uint i = 0; i < count; i++)
    {
        if ((strlen(set[i]) == length) && (strncmp(set[i], name, length) == 0))
            return true;
    }
    return false;
}

// The pseudo random numbers of the tests (xorshift64, fixed seed)
uint64 gRandom = 0x9E3779B97F4A7C15ULL;
uint64 nextRandom()
{
    gRandom ^= gRandom << 13;
    gRandom ^= gRandom >> 7;
    gRandom ^= gRandom << 17;
    return gRandom;
}

// The values the random inputs take one time in four
const uint64 EDGE_VALUES[] = {
    0, 1, 2, 3, ~(uint64)0, 0x8000000000000000ULL, 0x7FFFFFFFFFFFFFFFULL,
    0x80000000ULL, 0x7FFFFFFFULL, 0xFFFFFFFFULL, 0x100000000ULL,
    0xFFFFFFFF80000000ULL, 0x7FF8000000000000ULL, 0xFFF0000000000000ULL,
    0x7FF0000000000001ULL, 0x3FF0000000000000ULL, 0x7FC00000ULL,
    0x7F800000ULL, 0xFF800000ULL, 0x3F800000ULL, 0xFFFFFFFFFFFF8000ULL,
    0x7FFFFFFFFFFFFFFEULL
};
const uint EDGE_COUNT = sizeof(EDGE_VALUES) / sizeof(EDGE_VALUES[0]);

uint64 randomWord()
{
    const uint64 r = nextRandom();
    if ((r & 3) == 0)
        return EDGE_VALUES[(r >> 8) % EDGE_COUNT];
    return nextRandom();
}

// The inputs of one case, and the outputs the runs are compared on
struct CaseState
{
    uint64 m_x[31];
    uint64 m_sp;
    uint m_nzcv;
    uint64 m_zLo[32];
    uint64 m_zHi[32];
    uint64 m_fpcr;
    uint64 m_fpsr;
    uint8 m_window[WINDOW_SIZE];
};

// The setup of a row, parsed from its text ("x1=4000 v2=... mem=4000:0a0b")
struct RowSetup
{
    bool m_hasX[31];
    uint64 m_x[31];
    bool m_hasSp;
    uint64 m_sp;
    bool m_hasZ[32];
    uint64 m_zLo[32];
    uint64 m_zHi[32];
    bool m_hasNzcv;
    uint m_nzcv;
    uint64 m_fpcr;
    uint64 m_fpsr;
    bool m_hasMem[WINDOW_SIZE];
    uint8 m_mem[WINDOW_SIZE];
    // The name of a key the parser doesn't know ("" when all are known)
    const char* m_unknown;
    // The name of a memory byte outside the window ("" when all are inside)
    bool m_memOutside;
};

// The value of a hex digit, or -1
int hexDigit(char c)
{
    if ((c >= '0') && (c <= '9'))
        return c - '0';
    if ((c >= 'a') && (c <= 'f'))
        return c - 'a' + 10;
    if ((c >= 'A') && (c <= 'F'))
        return c - 'A' + 10;
    return -1;
}

// Parse the hex digits of [text, end) into a 128 bit value (the last digit is
// the lowest nibble)
void parseHex128(const char* text, const char* end, uint64& lo, uint64& hi)
{
    lo = 0;
    hi = 0;
    const uint digits = (uint)(end - text);
    for (uint i = 0; i < digits && i < 32; i++)
    {
        const uint64 nibble = (uint64)hexDigit(text[digits - 1 - i]);
        if (i < 16)
            lo |= nibble << (4 * i);
        else
            hi |= nibble << (4 * (i - 16));
    }
}

// Parse the setup text of a row. Return false on a key the parser doesn't know
bool parseSetup(const char* text, RowSetup& setup)
{
    memset(&setup, 0, sizeof(setup));
    setup.m_unknown = "";
    const char* p = text;
    while (*p != 0)
    {
        while (*p == ' ')
            p++;
        if (*p == 0)
            break;
        const char* end = p;
        while ((*end != 0) && (*end != ' '))
            end++;
        const char* equals = p;
        while ((equals < end) && (*equals != '='))
            equals++;
        if (equals == end)
        {
            setup.m_unknown = "a token without '='";
            return false;
        }
        const uint nameLength = (uint)(equals - p);
        const char* value = equals + 1;
        uint64 lo = 0, hi = 0;
        unsigned int index = 0;
        if ((nameLength == 3) && (strncmp(p, "mem", 3) == 0))
        {
            // addr:bytes[,addr:bytes...]
            const char* q = value;
            while (q < end)
            {
                uint64 address = 0;
                while ((q < end) && (*q != ':'))
                {
                    address = (address << 4) | (uint64)hexDigit(*q);
                    q++;
                }
                if (q == end)
                    break;
                q++;
                while ((q < end) && (*q != ','))
                {
                    const uint64 byte = ((uint64)hexDigit(q[0]) << 4) |
                                        (uint64)hexDigit(q[1]);
                    if ((address < WINDOW_BASE) ||
                        (address - WINDOW_BASE >= WINDOW_SIZE))
                        setup.m_memOutside = true;
                    else
                    {
                        setup.m_mem[address - WINDOW_BASE] = (uint8)byte;
                        setup.m_hasMem[address - WINDOW_BASE] = true;
                    }
                    address++;
                    q += 2;
                }
                if (q < end)
                    q++;
            }
        }
        else if ((nameLength >= 2) && (p[0] == 'x') &&
                 (sscanf(p + 1, "%u", &index) == 1) && (index < 31))
        {
            parseHex128(value, end, lo, hi);
            setup.m_hasX[index] = true;
            setup.m_x[index] = lo;
        }
        else if ((nameLength >= 2) && (p[0] == 'v') &&
                 (sscanf(p + 1, "%u", &index) == 1) && (index < 32))
        {
            parseHex128(value, end, lo, hi);
            setup.m_hasZ[index] = true;
            setup.m_zLo[index] = lo;
            setup.m_zHi[index] = hi;
        }
        else if ((nameLength == 2) && (strncmp(p, "sp", 2) == 0))
        {
            parseHex128(value, end, lo, hi);
            setup.m_hasSp = true;
            setup.m_sp = lo;
        }
        else if ((nameLength == 4) && (strncmp(p, "nzcv", 4) == 0))
        {
            parseHex128(value, end, lo, hi);
            setup.m_hasNzcv = true;
            setup.m_nzcv = (uint)lo;
        }
        else if ((nameLength == 4) && (strncmp(p, "fpcr", 4) == 0))
        {
            parseHex128(value, end, lo, hi);
            setup.m_fpcr = lo;
        }
        else if ((nameLength == 4) && (strncmp(p, "fpsr", 4) == 0))
        {
            parseHex128(value, end, lo, hi);
            setup.m_fpsr = lo;
        }
        else if ((nameLength == 2) && (strncmp(p, "pc", 2) == 0))
        {
            // The rows run at ROW_ADDRESS: the setup pc is not used
        }
        else
        {
            setup.m_unknown = "an unknown setup key";
            return false;
        }
        p = end;
    }
    return true;
}

#ifdef LOWERING_ARM64_NATIVE
// ----------------------------------------------------------------------
// The state of the native runs. The register ids map to the slots below:
//   0x100 the program counter, 0x110 the exit reason (the lowering contract)
//   0x1000 + 8 * i   x0-x30 (w0-w30 are their low halves)
//   0x1100           n, 0x1108 z, 0x1110 c, 0x1118 v (8 bytes each)
//   0x1120 sp, 0x1128 fpcr, 0x1130 fpsr
//   0x1400 + 16 * i  z0-z31 (and the v, q, d, s, h, b views of register i)
//   0x8000 + 8 * id  any other register (not used by the corpus)
//   0x10000 + 16 * n the temporaries
// The clear covers the first 0x10000 bytes (the temporaries are written
// before they are read, so they need no clear)
// ----------------------------------------------------------------------
const uint STATE_SIZE = 0x40000;
const uint STATE_CLEAR = 0x10000;
const uint STATE_X = 0x1000;
const uint STATE_FLAGS = 0x1100;
const uint STATE_SP = 0x1120;
const uint STATE_FPCR = 0x1128;
const uint STATE_FPSR = 0x1130;
const uint STATE_VECTOR = 0x1400;
const uint STATE_OTHER = 0x8000;
const uint STATE_TEMPORARY = 0x10000;
const uint STATE_PC = 0x100;
const uint STATE_EXIT = 0x110;

// The guest state of the native runs: z0 is 16 byte aligned
uint64 gState[STATE_SIZE / 8] __attribute__((aligned(16)));
uint8 gHostWindow[WINDOW_SIZE] __attribute__((aligned(16)));

// The state slot of a guest register (see the layout above)
uint stateOffset(uint guestRegister)
{
    using namespace armdis;
    if ((guestRegister >= ARM64_REG_X0) && (guestRegister <= ARM64_REG_X30))
        return STATE_X + (guestRegister - ARM64_REG_X0) * 8;
    if ((guestRegister >= ARM64_REG_W0) && (guestRegister <= ARM64_REG_W30))
        return STATE_X + (guestRegister - ARM64_REG_W0) * 8;
    if (guestRegister == ARM64_REG_SP)
        return STATE_SP;
    if (guestRegister == ARM64_REG_N)
        return STATE_FLAGS;
    if (guestRegister == ARM64_REG_Z)
        return STATE_FLAGS + 8;
    if (guestRegister == ARM64_REG_C)
        return STATE_FLAGS + 16;
    if (guestRegister == ARM64_REG_V)
        return STATE_FLAGS + 24;
    if (guestRegister == ARM64_REG_FPCR)
        return STATE_FPCR;
    if (guestRegister == ARM64_REG_FPSR)
        return STATE_FPSR;
    // The vector registers and their views share the slot of their number
    if ((guestRegister >= ARM64_REG_Z0) && (guestRegister <= ARM64_REG_Z31))
        return STATE_VECTOR + (guestRegister - ARM64_REG_Z0) * 16;
    if ((guestRegister >= ARM64_REG_V0) && (guestRegister <= ARM64_REG_V31))
        return STATE_VECTOR + (guestRegister - ARM64_REG_V0) * 16;
    if ((guestRegister >= ARM64_REG_Q0) && (guestRegister <= ARM64_REG_Q31))
        return STATE_VECTOR + (guestRegister - ARM64_REG_Q0) * 16;
    if ((guestRegister >= ARM64_REG_D0) && (guestRegister <= ARM64_REG_D31))
        return STATE_VECTOR + (guestRegister - ARM64_REG_D0) * 16;
    if ((guestRegister >= ARM64_REG_S0) && (guestRegister <= ARM64_REG_S31))
        return STATE_VECTOR + (guestRegister - ARM64_REG_S0) * 16;
    if ((guestRegister >= ARM64_REG_H0) && (guestRegister <= ARM64_REG_H31))
        return STATE_VECTOR + (guestRegister - ARM64_REG_H0) * 16;
    if ((guestRegister >= ARM64_REG_B0) && (guestRegister <= ARM64_REG_B31))
        return STATE_VECTOR + (guestRegister - ARM64_REG_B0) * 16;
    return STATE_OTHER + guestRegister * 8;
}

// The register map of the tests: the state of the A64 guest
class Arm64StateMap : public SemanticRegisterMap {
public:
    virtual bool getTargetRegister(uint, uint&) const { return false; }
    virtual uint getStateOffset(uint guestRegister) const
    {
        return stateOffset(guestRegister);
    }
    virtual uint getStateRegister() const { return ia32dis::AMD64_REG_R15; }
    virtual uint getTemporaryOffset(uint temporary) const
    {
        return STATE_TEMPORARY + temporary * 16;
    }
    virtual uint getMemoryBaseRegister() const { return ia32dis::AMD64_REG_R14; }
    virtual uint getProgramCounterOffset() const { return STATE_PC; }
    virtual uint getExitReasonOffset() const { return STATE_EXIT; }
};

// The stack of the first pass binary of the assembler of the tests
class Arm64LoweringStack : public StackInterface {
public:
    Arm64LoweringStack(FirstPassBinaryPtr& binary) :
        StackInterface(*binary, true, 4) {}
    virtual uint getStackType() { return 4; }
    virtual uint getRegisterSize() { return 4; }
    virtual RegisterAllocationTable& getRegistersTable() { return m_registers; }
    virtual const RegisterAllocationTable& getRegistersTable() const { return m_registers; }
    virtual StackLocation replaceRegisterToStackVariable(StackLocation source)
    {
        return source;
    }
    RegisterAllocationTable m_registers;
};

// The native function: the state in rdi, the memory bias in rsi
typedef void (*NativeFunction)(void* state, void* bias);

// The prologue and the epilogue of the native function (the exit label)
const char* const PROLOGUE =
    "push rbx\npush rbp\npush r12\npush r13\npush r14\npush r15\n"
    "mov r15, rdi\nmov r14, rsi\n";
const char* const EPILOGUE =
    "pop r15\npop r14\npop r13\npop r12\npop rbp\npop rbx\nret\n";

// Assemble a text into bytes. Return false on an assembler error
bool assembleBytes(const char* text, cBuffer& code)
{
    FirstPassBinaryPtr binary(new FirstPassBinary(
                            OpcodeSubsystems::DISASSEMBLER_AMD_64, false));
    binary->createNewBlockWithoutChange(0,
            StackInterfacePtr(new Arm64LoweringStack(binary)));
    binary->changeBasicBlock(0);
    AssemblerInterfacePtr assembler = AssemblingFactory::generateAssembler(
                            binary, OpcodeSubsystems::DISASSEMBLER_AMD_64);
    cStringerStreamPtr stream = assembler->getAssembler();
    *stream << cString(text);
    IA32AssemblerStream* assembly =
                dynamic_cast<IA32AssemblerStream*>(stream.getPointer());
    if (assembly == NULL)
        return false;
    XSTL_TRY
    {
        assembly->assemble();
    }
    XSTL_CATCH(cException&)
    {
        return false;
    }
    const cBuffer& data = binary->getCurrentBlockData();
    code = cBuffer(data.getSize());
    for (uint i = 0; i < data.getSize(); i++)
        code[i] = data[i];
    return true;
}

// Write a 32 bit little endian value at 'offset'
void patch32(cBuffer& code, uint offset, int32 value)
{
    for (uint b = 0; b < 4; b++)
        code[offset + b] = (uint8)((uint32)value >> (8 * b));
}

// Lower a legalized block and link it into executable memory: the prologue,
// the lowered code, a stub for each guest label the block leaves to (the next
// program counter, the reason REASON_LABEL, then the epilogue) and the
// epilogue. Return false when the host won't map executable memory
bool buildNative(const SemanticBlock& block, const SemanticRegisterMap& registerMap,
                 NativeFunction& function, void*& memory, uint& memorySize)
{
    AMD64Lowering lowering;
    AMD64Lowering::LoweredBlock lowered;
    lowering.lowerBlock(block, registerMap, lowered);

    cBuffer prologue, epilogue;
    if (!assembleBytes(PROLOGUE, prologue) || !assembleBytes(EPILOGUE, epilogue))
        return false;

    // The stubs, one for each guest label the block refers to
    cList<cString> names;
    for (cList<AMD64Lowering::LabelDependency>::iterator d =
            lowered.m_dependencies.begin();
         d != lowered.m_dependencies.end(); d++)
    {
        bool isKnown = (*d).m_name == AMD64Lowering::getExitLabel();
        for (cList<cString>::iterator n = names.begin(); n != names.end(); n++)
            isKnown = isKnown || (*n == (*d).m_name);
        if (!isKnown)
            names.append((*d).m_name);
    }
    const uint codeOffset = prologue.getSize();
    uint offset = codeOffset + lowered.m_code.getSize();
    cList<uint> stubOffsets;
    cList<cBuffer> stubs;
    for (cList<cString>::iterator n = names.begin(); n != names.end(); n++)
    {
        // The name is B_<hex>: the next program counter, then the label reason
        cSArray<char> label = (*n).getASCIIstring();
        char text[300];
        snprintf(text, sizeof(text),
                 "mov rax, 0x%s\nmov qword [r15+0x100], rax\n"
                 "mov qword [r15+0x110], 0x%llx\n", label.getBuffer() + 2,
                 (unsigned long long)REASON_LABEL);
        cBuffer stub;
        if (!assembleBytes(text, stub))
            return false;
        stubOffsets.append(offset);
        stubs.append(stub);
        offset += stub.getSize() + 5;
    }
    const uint epilogueOffset = offset;

    const uint size = epilogueOffset + epilogue.getSize();
    cBuffer code(size);
    for (uint i = 0; i < codeOffset; i++)
        code[i] = prologue[i];
    for (uint i = 0; i < lowered.m_code.getSize(); i++)
        code[codeOffset + i] = lowered.m_code[i];
    cList<uint>::iterator so = stubOffsets.begin();
    for (cList<cBuffer>::iterator s = stubs.begin(); s != stubs.end(); s++, so++)
    {
        for (uint i = 0; i < (*s).getSize(); i++)
            code[*so + i] = (*s)[i];
        // jmp epilogue
        const uint jump = *so + (*s).getSize();
        code[jump] = 0xE9;
        patch32(code, jump + 1, (int32)(epilogueOffset - (jump + 5)));
    }
    for (uint i = 0; i < epilogue.getSize(); i++)
        code[epilogueOffset + i] = epilogue[i];

    // Resolve the references of the lowered code
    for (cList<AMD64Lowering::LabelDependency>::iterator d =
            lowered.m_dependencies.begin();
         d != lowered.m_dependencies.end(); d++)
    {
        uint target = epilogueOffset;
        if (!((*d).m_name == AMD64Lowering::getExitLabel()))
        {
            target = 0;
            cList<uint>::iterator so2 = stubOffsets.begin();
            for (cList<cString>::iterator n = names.begin(); n != names.end();
                 n++, so2++)
            {
                if (*n == (*d).m_name)
                    target = *so2;
            }
        }
        const uint field = codeOffset + (*d).m_offset;
        patch32(code, field, (int32)(target - (field + 4)));
    }

    memory = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED)
    {
        memory = NULL;
        return false;
    }
    memorySize = size;
    for (uint i = 0; i < size; i++)
        ((uint8*)memory)[i] = code[i];
    function = reinterpret_cast<NativeFunction>(memory);
    return true;
}

// The guest memory of the interpreter: the window only (any other address
// is a memory fault, which the rows must not reach)
class WindowMemory : public SemanticMemory {
public:
    WindowMemory(const uint8* window)
    {
        memcpy(m_bytes, window, WINDOW_SIZE);
    }
    virtual bool read(uint64 address, uint8* buffer, uint size)
    {
        if (!inWindow(address, size))
            return false;
        memcpy(buffer, m_bytes + (address - WINDOW_BASE), size);
        return true;
    }
    virtual bool write(uint64 address, const uint8* buffer, uint size)
    {
        if (!inWindow(address, size))
            return false;
        memcpy(m_bytes + (address - WINDOW_BASE), buffer, size);
        return true;
    }
    uint8 m_bytes[WINDOW_SIZE];
private:
    static bool inWindow(uint64 address, uint size)
    {
        return (address >= WINDOW_BASE) &&
               (address - WINDOW_BASE + size <= WINDOW_SIZE);
    }
};

// Is the lane of 'bits' bits (16, 32 or 64) at the bottom of the 128 bit value
// a NaN
bool isNaNLane(uint64 lo, uint bits)
{
    if (bits == 16)
        return ((lo & 0x7C00) == 0x7C00) && ((lo & 0x3FF) != 0);
    if (bits == 32)
        return ((lo & 0x7F800000ULL) == 0x7F800000ULL) && ((lo & 0x7FFFFFULL) != 0);
    return ((lo & 0x7FF0000000000000ULL) == 0x7FF0000000000000ULL) &&
           ((lo & 0x000FFFFFFFFFFFFFULL) != 0);
}

// Are the bits from 'bits' up of two 128 bit values equal
bool upperEqual(uint64 expectedLo, uint64 expectedHi, uint64 actualLo,
                uint64 actualHi, uint bits)
{
    if (bits == 64)
        return expectedHi == actualHi;
    if (bits == 32)
        return ((expectedLo >> 32) == (actualLo >> 32)) && (expectedHi == actualHi);
    return ((expectedLo >> 16) == (actualLo >> 16)) && (expectedHi == actualHi);
}

// The width of the scalar floating point result of a row (0 when the row has
// no NaN rule: integer and vector results, and the comparisons)
uint nanWidth(const char* text, uint mnemonicLen)
{
    if (text[0] != 'f')
        return 0;
    if ((strncmp(text, "fcmp", 4) == 0) || (strncmp(text, "fccmp", 5) == 0))
        return 0;
    if (mnemonicLen >= 4 && (strncmp(text, "fmov", 4) == 0))
        return 0;
    // FMINNM and FMAXNM return an operand: a NaN result is that operand (quieted,
    // its sign and payload kept), so its bits are compared exactly, which is what
    // tells the winner of two NaNs apart
    if ((strncmp(text, "fmaxnm", 6) == 0) || (strncmp(text, "fminnm", 6) == 0))
        return 0;
    // The fused multiply-add follows the ARM NaN rule exactly: the
    // first signaling NaN of (a, n, m), else the first NaN, quieted with its
    // sign and payload; the default NaN (positive) for an invalid product
    if ((strncmp(text, "fmadd", 5) == 0) || (strncmp(text, "fmsub", 5) == 0) ||
        (strncmp(text, "fnmadd", 6) == 0) || (strncmp(text, "fnmsub", 6) == 0))
        return 0;
    const char* operand = text + mnemonicLen;
    while (*operand == ' ')
        operand++;
    switch (*operand)
    {
        case 'h': return 16;
        case 's': return 32;
        case 'd': return 64;
        default: return 0;
    }
}

// Does the text have a half precision register operand (h0-h31)
bool hasHalfOperand(const char* text)
{
    for (const char* p = text; *p != 0; p++)
    {
        if ((p[0] == 'h') && (p[1] >= '0') && (p[1] <= '9') &&
            ((p == text) || (p[-1] == ' ')))
            return true;
    }
    return false;
}

// The outcome of one row, for its mnemonic
enum RowStatus { ROW_PASSED, ROW_FAILED, ROW_SKIPPED };

class TestObjectTestSemanticLoweringArm64 : public cTestObject {
public:
    TestObjectTestSemanticLoweringArm64() : m_run(0),
                                            m_failed(0), m_skipped(0)
    {
        for (uint i = 0; i < MNEMONIC_COUNT; i++)
        {
            m_passed[i] = 0;
            m_rowSkipped[i] = 0;
        }
    }

    virtual void test()
    {
        ArchitectureDescriptionPtr guest(new Arm64Architecture(128, 128));
        ArchitectureDescriptionPtr target(
                ArchitectureDescriptionFactory::getArchitecture(
                                    OpcodeSubsystems::DISASSEMBLER_AMD_64));
        m_guest = guest;
        m_target = target;

        // The rows of the semantics corpus (every mnemonic of the set)
        static const CorpusRow semantic[] = { ARM64_SEMANTIC_ROWS };
        for (uint i = 0; i < sizeof(semantic) / sizeof(semantic[0]); i++)
            considerCorpus(semantic[i], NULL, 0, false);
        // The rows of the static musl guests: the thread pointer and the zero block
        // size system registers, the monitor of a lone stlxr (status 1, the
        // memory unchanged). Each mnemonic has its own passing row here
        static const CorpusRow phase4[] = { PHASE4_ROWS };
        for (uint i = 0; i < sizeof(phase4) / sizeof(phase4[0]); i++)
            considerCorpus(phase4[i], NULL, 0, false);
        // The NaN rows of FMINNM and FMAXNM: signaling and quiet NaN
        // operands, the NaN of either side, and the signed zeros
        static const CorpusRow nanRows[] = { ARM_NAN_ROWS };
        for (uint i = 0; i < sizeof(nanRows) / sizeof(nanRows[0]); i++)
            considerCorpus(nanRows[i], NULL, 0, false);
        // The fused multiply-add rows: the ARM NaN rule of the product-add
        // and the cancellation of the fused product, double and single
        static const CorpusRow fusedRows[] = { ARM_FUSED_ROWS };
        for (uint i = 0; i < sizeof(fusedRows) / sizeof(fusedRows[0]); i++)
            considerCorpus(fusedRows[i], NULL, 0, false);
        // The scalar floating point rows (no arrangement)
        static const CorpusRow simdFloat[] = { ARM64_SIMD_ROWS_FLOAT };
        for (uint i = 0; i < sizeof(simdFloat) / sizeof(simdFloat[0]); i++)
            considerCorpus(simdFloat[i], NULL, 0, true);
        // The population count, add across and move immediate rows (the
        // other integer SIMD rows are vector forms of the mnemonics above)
        static const CorpusRow simdInteger[] = { ARM64_SIMD_ROWS_INTEGER };
        for (uint i = 0; i < sizeof(simdInteger) / sizeof(simdInteger[0]); i++)
            considerCorpus(simdInteger[i], SIMD_INTEGER_MNEMONICS,
                           SIMD_INTEGER_COUNT, false);
        // The scalar rows of the disassembler corpus, for the mnemonics the
        // semantics corpus doesn't have in scalar form
        static const TextRow3 text3[] = { ARM64_TEXT_ROWS };
        for (uint i = 0; i < sizeof(text3) / sizeof(text3[0]); i++)
            considerText(text3[i].m_word, text3[i].m_text, TEXT_MNEMONICS,
                         sizeof(TEXT_MNEMONICS) / sizeof(TEXT_MNEMONICS[0]));
        static const TextRow4 text4[] = { ARM64_SIMD_TEXT_ROWS };
        for (uint i = 0; i < sizeof(text4) / sizeof(text4[0]); i++)
            considerText(text4[i].m_word, text4[i].m_text, TEXT4_MNEMONICS,
                         sizeof(TEXT4_MNEMONICS) / sizeof(TEXT4_MNEMONICS[0]));

        cout << "arm64 rows: " << m_run << " run, " << m_failed << " failed, "
             << m_skipped << " skipped" << endl;
        for (uint i = 0; i < MNEMONIC_COUNT; i++)
        {
            cout << "arm64 coverage: " << MNEMONICS[i] << " " << m_passed[i]
                 << " passed, " << m_rowSkipped[i] << " skipped" << endl;
        }
        TESTS_ASSERT(m_failed == 0);
        for (uint i = 0; i < MNEMONIC_COUNT; i++)
        {
            if (m_passed[i] == 0)
                fprintf(stderr, "TestSemanticLoweringArm64: no passing row of %s\n",
                        MNEMONICS[i]);
            TESTS_ASSERT(m_passed[i] != 0);
        }
        cout << "Semantic lowering arm64 tests passed" << endl;
    }

    // Return the name of the module
    virtual cString getName() { return __FILE__; }

private:
    // A corpus row of the mnemonic set: when 'names' is given, only its
    // mnemonics. 'scalarOnly' rows have no arrangement (no '.' and digit)
    void considerCorpus(const CorpusRow& corpus, const char* const* names,
                        uint nameCount, bool scalarOnly)
    {
        const uint length = mnemonicLength(corpus.m_text);
        const int index = mnemonicIndex(corpus.m_text, length);
        if (index < 0)
            return;
        if ((names != NULL) && !inSet(corpus.m_text, length, names, nameCount))
            return;
        if (scalarOnly && isVectorText(corpus.m_text))
            return;
        RowInfo row;
        row.m_count = corpus.m_count;
        for (uint i = 0; i < 4; i++)
            row.m_words[i] = corpus.m_words[i];
        row.m_text = corpus.m_text;
        row.m_setup = corpus.m_setup;
        row.m_expected = corpus.m_expected;
        row.m_index = (uint)index;
        runRow(row);
    }

    // A disassembler row (no setup) of the mnemonics of 'set'. It is scalar
    void considerText(uint32 word, const char* text, const char* const* set,
                      uint count)
    {
        const uint length = mnemonicLength(text);
        const int index = mnemonicIndex(text, length);
        if ((index < 0) || !inSet(text, length, set, count) || isVectorText(text))
            return;
        RowInfo row;
        row.m_count = 1;
        row.m_words[0] = word;
        row.m_words[1] = row.m_words[2] = row.m_words[3] = 0;
        row.m_text = text;
        row.m_setup = "";
        row.m_expected = NULL;
        row.m_index = (uint)index;
        runRow(row);
    }

    struct RowInfo
    {
        uint32 m_words[4];
        uint m_count;
        const char* m_text;
        const char* m_setup;
        const char* m_expected;
        uint m_index;
    };

    // Lift, legalize, lower and run one row. Count the outcome
    void runRow(const RowInfo& row)
    {
        const char* skipReason = NULL;
        RowStatus status = checkRow(row, skipReason);
        if (status == ROW_SKIPPED)
        {
            m_skipped++;
            m_rowSkipped[row.m_index]++;
            cout << "arm64 skip: " << row.m_text << ": " << skipReason << endl;
            return;
        }
        m_run++;
        if (status == ROW_PASSED)
            m_passed[row.m_index]++;
        else
            m_failed++;
    }

    // The check of a row. A skipped row sets reason
    RowStatus checkRow(const RowInfo& row, const char*& skipReason)
    {
        const uint length = mnemonicLength(row.m_text);
        const bool isMemory = (length >= 2) &&
            (((row.m_text[0] == 'l') && (row.m_text[1] == 'd')) ||
             ((row.m_text[0] == 's') && (row.m_text[1] == 't')));
        // A literal load reads the code (outside the window): not a memory row
        // of this test
        if (isMemory && (strstr(row.m_text, "[") == NULL) &&
            (strncmp(row.m_text, "ld", 2) == 0))
        {
            skipReason = "a literal load reads the code, outside the guest memory window";
            return ROW_SKIPPED;
        }

        // The half precision rows: the legalizer refuses the 16-bit operations
        // (the Lua guest uses none; the float lowering covers single and double)
        const bool isFloatMnemonic = (row.m_text[0] == 'f') ||
            (strncmp(row.m_text, "scvtf", 5) == 0) || (strncmp(row.m_text, "ucvtf", 5) == 0);
        if (isFloatMnemonic && hasHalfOperand(row.m_text))
        {
            skipReason = "half precision: the legalizer and the lowering refuse the 16-bit operations (the Lua guest uses none)";
            return ROW_SKIPPED;
        }

        // A row which expects a guest memory fault: the lowered code has no
        // bounds check (a fault is the runtime's, not the block's)
        if ((row.m_expected != NULL) && (strstr(row.m_expected, "exit=fault") != NULL))
        {
            skipReason = "the corpus expects a guest memory fault, which the lowered code doesn't check";
            return ROW_SKIPPED;
        }

        // The system registers the lowering has code for: NZCV, FPCR, FPSR,
        // TPIDR_EL0, TPIDRRO_EL0 and DCZID_EL0 (the runtime serves these; the
        // guests of the tests read no other). The others are a helper call
        // the lowering refuses, or an encoding the legalizer refuses
        if ((strncmp(row.m_text, "mrs ", 4) == 0) || (strncmp(row.m_text, "msr ", 4) == 0))
        {
            if (!isLoweredSystemRegister(row.m_text))
            {
                skipReason = "a system register or PSTATE field the lowering has no code for (the runtime serves NZCV, FPCR, FPSR, TPIDR_EL0, TPIDRRO_EL0 and DCZID_EL0)";
                return ROW_SKIPPED;
            }
        }
        // Cache maintenance: dc zva is lowered (the zero block); the others
        // (civac, cvac, ivac...) have no lowering yet, and the Lua guest uses none
        if ((strncmp(row.m_text, "dc ", 3) == 0) && (strncmp(row.m_text, "dc zva", 6) != 0))
        {
            skipReason = "cache maintenance other than dc zva: no lowering (the guests of the tests use dc zva only)";
            return ROW_SKIPPED;
        }

        RowSetup setup;
        if (!parseSetup(row.m_setup, setup))
        {
            fprintf(stderr, "TestSemanticLoweringArm64: %s: %s\n", row.m_text,
                    setup.m_unknown);
            return ROW_FAILED;
        }
        if (setup.m_memOutside)
        {
            skipReason = "the setup memory is outside the guest memory window";
            return ROW_SKIPPED;
        }

        // The lift
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE, m_guest);
        for (uint i = 0; i < row.m_count; i++)
        {
            ArmOpcodePtr decoded = ArmStreamDisassembler::decode(
                    ArmInstructionSet::ARM_A64, row.m_words[i], 4,
                    ROW_ADDRESS + 4 * i, ArmFeatureSet::all());
            if (decoded.getPointer() == NULL)
                return reportFailure(row, "the word doesn't decode");
            OpcodePtr opcode((Opcode*)decoded.getPointer(), SMARTPTR_DESTRUCT_NONE);
            if (!lifter.lift(opcode, block))
                return reportFailure(row, "the word doesn't lift");
        }
        block.setFallThroughAddress(ProcessorAddress(
                ProcessorAddress::PROCESSOR_64, ROW_ADDRESS + 4 * row.m_count));

        // The legalized copy, lowered and linked
        SemanticBlock legalized(block);
        try
        {
            SemanticLegalizer legalizer(m_guest, m_target);
            legalizer.legalize(legalized);
        } catch (cException& exception)
        {
            return reportFailure(row, cString(exception.getMessage()).getASCIIstring().getBuffer());
        }
        Arm64StateMap registerMap;
        NativeFunction function = NULL;
        void* memory = NULL;
        uint memorySize = 0;
        try
        {
            if (!buildNative(legalized, registerMap, function, memory, memorySize))
            {
                skipReason = "no executable memory (the host won't map it)";
                return ROW_SKIPPED;
            }
        } catch (cException& exception)
        {
            return reportFailure(row, cString(exception.getMessage()).getASCIIstring().getBuffer());
        }

        // The cases: the setup first, then the random inputs
        const uint nanW = nanWidth(row.m_text, length);
        // dc zva stores the block of its address register: a memory row too
        // (the LSE atomics, cas and swp, address their register too)
        const bool memoryRow = isMemory || (strncmp(row.m_text, "dc ", 3) == 0) ||
            (strncmp(row.m_text, "cas", 3) == 0) || (strncmp(row.m_text, "swp", 3) == 0);
        bool isPassed = true;
        for (uint c = 0; (c < CASES_PER_ROW) && isPassed; c++)
        {
            CaseState state;
            makeCase(row, setup, c, memoryRow, state);
            if (!compareCase(row, block, function, state, nanW))
                isPassed = false;
        }
        munmap(memory, memorySize);
        if (!isPassed)
            return ROW_FAILED;
        return ROW_PASSED;
    }

    // Does the text of an mrs or msr name one of the system registers the
    // lowering has code for (case of the text does not matter)?
    static bool isLoweredSystemRegister(const char* text)
    {
        static const char* const NAMES[] = {
            "nzcv", "fpcr", "fpsr", "tpidr_el0", "tpidrro_el0", "dczid_el0"
        };
        char lower[128];
        uint length = 0;
        for (const char* p = text; (*p != 0) && (length < sizeof(lower) - 1); p++)
            lower[length++] = (char)((*p >= 'A') && (*p <= 'Z') ? *p + 32 : *p);
        lower[length] = 0;
        for (uint i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++)
        {
            if (strstr(lower, NAMES[i]) != NULL)
                return true;
        }
        return false;
    }

    RowStatus reportFailure(const RowInfo& row, const char* message)
    {
        fprintf(stderr, "TestSemanticLoweringArm64: %s: %s\n", row.m_text, message);
        return ROW_FAILED;
    }

    // The inputs of one case. Case 0 is the setup (zeros where it has no
    // value). The others are random, the memory rows keep their base
    // registers in the window
    void makeCase(const RowInfo& row, const RowSetup& setup, uint c,
                  bool memoryRow, CaseState& state)
    {
        const bool random = (c != 0);
        // The registers of a memory row: the setup value, the window base for
        // a base register of the text, else zero
        const uint64 defaultSp = 0x8000;
        for (uint i = 0; i < 31; i++)
        {
            if (setup.m_hasX[i] && !(random && !memoryRow))
                state.m_x[i] = setup.m_x[i];
            else if (memoryRow)
                state.m_x[i] = isBaseRegister(row.m_text, i) ? WINDOW_DEFAULT_BASE : 0;
            else
                state.m_x[i] = random ? randomWord() : 0;
        }
        if (setup.m_hasSp && !(random && !memoryRow))
            state.m_sp = setup.m_sp;
        else if (memoryRow)
            state.m_sp = isBaseRegister(row.m_text, 31) ? WINDOW_DEFAULT_BASE : 0;
        else
            state.m_sp = random ? randomWord() : defaultSp;
        if (setup.m_hasNzcv && !random)
            state.m_nzcv = setup.m_nzcv;
        else
            state.m_nzcv = random ? (uint)(nextRandom() & 15) : 0;
        for (uint i = 0; i < 32; i++)
        {
            if (setup.m_hasZ[i] && !random)
            {
                state.m_zLo[i] = setup.m_zLo[i];
                state.m_zHi[i] = setup.m_zHi[i];
            } else
            {
                state.m_zLo[i] = random ? randomWord() : 0;
                state.m_zHi[i] = random ? randomWord() : 0;
            }
        }
        // The lowering assumes FPCR = 0 (round to nearest, no flush to zero,
        // default NaN off): the Lua guest and the runtime run with it. The
        // setup's FPCR is ignored, and FPSR (the cumulative flags, not kept by
        // the lowering) is not compared
        state.m_fpcr = 0;
        state.m_fpsr = 0;
        for (uint i = 0; i < WINDOW_SIZE; i++)
        {
            if (random)
                state.m_window[i] = (uint8)nextRandom();
            else
                state.m_window[i] = setup.m_hasMem[i] ? setup.m_mem[i] : 0;
        }
    }

    // Is register 'index' (31 for sp) the base of a memory operand of the text
    // (the address of dc zva, the last operand, counts as one)
    static bool isBaseRegister(const char* text, uint index)
    {
        if ((strncmp(text, "dc ", 3) == 0) && (index != 31))
        {
            const char* last = strrchr(text, ',');
            unsigned int value = 0;
            if ((last != NULL) && (sscanf(last + 1, " x%u", &value) == 1) &&
                (value == index))
                return true;
            return false;
        }
        for (const char* p = text; *p != 0; p++)
        {
            if (*p != '[')
                continue;
            const char* q = p + 1;
            if (index == 31)
            {
                if (strncmp(q, "sp", 2) == 0)
                    return true;
                continue;
            }
            if ((q[0] == 'x') || (q[0] == 'w'))
            {
                unsigned int value = 0;
                if ((sscanf(q + 1, "%u", &value) == 1) && (value == index))
                    return true;
            }
        }
        return false;
    }

    // Run one case in both: the interpreter on the lifted block, the native
    // code on the lowered one. Compare the outcomes
    bool compareCase(const RowInfo& row, const SemanticBlock& block,
                     NativeFunction function, const CaseState& state, uint nanW)
    {
        using namespace armdis;
        char message[512];
        WindowMemory memory(state.m_window);
        SemanticInterpreter interpreter(m_guest, memory);
        for (uint i = 0; i < 31; i++)
            interpreter.setRegister(ARM64_REG_X0 + i, SemanticValue(state.m_x[i]));
        interpreter.setRegister(ARM64_REG_SP, SemanticValue(state.m_sp));
        interpreter.setRegister(ARM64_REG_PC, SemanticValue(ROW_ADDRESS));
        interpreter.setRegister(ARM64_REG_N, SemanticValue((state.m_nzcv >> 3) & 1));
        interpreter.setRegister(ARM64_REG_Z, SemanticValue((state.m_nzcv >> 2) & 1));
        interpreter.setRegister(ARM64_REG_C, SemanticValue((state.m_nzcv >> 1) & 1));
        interpreter.setRegister(ARM64_REG_V, SemanticValue(state.m_nzcv & 1));
        interpreter.setRegister(ARM64_REG_FPCR, SemanticValue(state.m_fpcr));
        interpreter.setRegister(ARM64_REG_FPSR, SemanticValue(state.m_fpsr));
        for (uint i = 0; i < 32; i++)
        {
            SemanticValue value(state.m_zLo[i]);
            value.setWord(1, state.m_zHi[i]);
            interpreter.setRegister(ARM64_REG_Z0 + i, value);
        }
        const SemanticInterpreter::ExitReason reason = interpreter.run(block);

        // The native state
        memset(gState, 0, STATE_CLEAR);
        uint8* bytes = reinterpret_cast<uint8*>(gState);
        for (uint i = 0; i < 31; i++)
            memcpy(bytes + stateOffset(ARM64_REG_X0 + i), &state.m_x[i], 8);
        memcpy(bytes + STATE_SP, &state.m_sp, 8);
        const uint64 flags[4] = {
            (state.m_nzcv >> 3) & 1, (state.m_nzcv >> 2) & 1,
            (state.m_nzcv >> 1) & 1, state.m_nzcv & 1
        };
        memcpy(bytes + STATE_FLAGS, flags, sizeof(flags));
        memcpy(bytes + STATE_FPCR, &state.m_fpcr, 8);
        memcpy(bytes + STATE_FPSR, &state.m_fpsr, 8);
        for (uint i = 0; i < 32; i++)
        {
            memcpy(bytes + STATE_VECTOR + i * 16, &state.m_zLo[i], 8);
            memcpy(bytes + STATE_VECTOR + i * 16 + 8, &state.m_zHi[i], 8);
        }
        memcpy(gHostWindow, state.m_window, WINDOW_SIZE);
        const void* bias = reinterpret_cast<const void*>(
                (uintptr_t)gHostWindow - (uintptr_t)WINDOW_BASE);
        function(gState, const_cast<void*>(bias));

        // The exit: a label (the reason of the label stub), a branch, a call
        // or a return to a register (the lowering's reasons 1-3), or a trap
        const uint64 exitReason = *(uint64*)(bytes + STATE_EXIT);
        const uint64 exitPc = *(uint64*)(bytes + STATE_PC);
        if (!compareExit(row, reason, interpreter, exitReason, exitPc))
            return false;

        // The integer registers and the flags
        for (uint i = 0; i < 31; i++)
        {
            const uint64 actual = *(uint64*)(bytes + stateOffset(ARM64_REG_X0 + i));
            const uint64 expected = interpreter.getRegister(ARM64_REG_X0 + i).getWord(0);
            if (actual != expected)
            {
                snprintf(message, sizeof(message), "x%llu differs: native 0x%llx interpreter 0x%llx",
                         (unsigned long long)i, (unsigned long long)actual, (unsigned long long)expected);
                return reportCase(row, message);
            }
        }
        {
            const uint64 actual = *(uint64*)(bytes + STATE_SP);
            const uint64 expected = interpreter.getRegister(ARM64_REG_SP).getWord(0);
            if (actual != expected)
            {
                snprintf(message, sizeof(message), "sp differs: native 0x%llx interpreter 0x%llx",
                         (unsigned long long)actual, (unsigned long long)expected);
                return reportCase(row, message);
            }
        }
        const uint kinds[4] = { ARM64_REG_N, ARM64_REG_Z, ARM64_REG_C, ARM64_REG_V };
        const char* names[4] = { "n", "z", "c", "v" };
        for (uint f = 0; f < 4; f++)
        {
            const uint64 actual = *(uint64*)(bytes + STATE_FLAGS + 8 * f) & 1;
            const uint64 expected = interpreter.getRegister(kinds[f]).getWord(0) & 1;
            if (actual != expected)
            {
                snprintf(message, sizeof(message), "flag %s differs: native %llu interpreter %llu",
                         names[f], (unsigned long long)actual, (unsigned long long)expected);
                return reportCase(row, message);
            }
        }

        // The vector registers: the low 128 bits, the NaN class of a scalar
        // floating point result
        for (uint i = 0; i < 32; i++)
        {
            const uint64 actualLo = *(uint64*)(bytes + STATE_VECTOR + i * 16);
            const uint64 actualHi = *(uint64*)(bytes + STATE_VECTOR + i * 16 + 8);
            const SemanticValue value = interpreter.getRegister(ARM64_REG_Z0 + i);
            const uint64 expectedLo = value.getWord(0);
            const uint64 expectedHi = value.getWord(1);
            if ((actualLo == expectedLo) && (actualHi == expectedHi))
                continue;
            // (a NaN result may be in any register: the rule is per register)
            const bool nanClass = (nanW != 0) &&
                isNaNLane(actualLo, nanW) && isNaNLane(expectedLo, nanW) &&
                upperEqual(expectedLo, expectedHi, actualLo, actualHi, nanW);
            if (!nanClass)
            {
                snprintf(message, sizeof(message),
                         "v%llu differs: native 0x%016llx%016llx interpreter 0x%016llx%016llx"
                         " (inputs x0 0x%llx x1 0x%llx v0 0x%016llx%016llx v1 0x%016llx%016llx"
                         " v2 0x%016llx%016llx)",
                         (unsigned long long)i, (unsigned long long)actualHi, (unsigned long long)actualLo,
                         (unsigned long long)expectedHi, (unsigned long long)expectedLo,
                         (unsigned long long)state.m_x[0], (unsigned long long)state.m_x[1],
                         (unsigned long long)state.m_zHi[0], (unsigned long long)state.m_zLo[0],
                         (unsigned long long)state.m_zHi[1], (unsigned long long)state.m_zLo[1],
                         (unsigned long long)state.m_zHi[2], (unsigned long long)state.m_zLo[2]);
                return reportCase(row, message);
            }
        }

        // The memory window
        if (memcmp(gHostWindow, memory.m_bytes, WINDOW_SIZE) != 0)
            return reportCase(row, "the memory window differs");
        return true;
    }

    // The exit the lowering must take for a mnemonic. A constant branch or call
    // is a plain jump to its label (the native exit is the label stub's reason);
    // a register branch exits with its reason, a call and a return too
    static uint64 expectedExitReason(const char* text)
    {
        const uint length = mnemonicLength(text);
        if ((length == 3) && (strncmp(text, "brk", 3) == 0))
            return AMD64Lowering::EXIT_TRAP;
        if ((length == 3) && (strncmp(text, "blr", 3) == 0))
            return AMD64Lowering::EXIT_CALL;
        if ((length == 3) && (strncmp(text, "ret", 3) == 0))
            return AMD64Lowering::EXIT_RETURN;
        if ((length == 2) && (strncmp(text, "br", 2) == 0))
            return AMD64Lowering::EXIT_BRANCH;
        return REASON_LABEL;
    }

    // The exits of the two runs: the native reason must be the one the mnemonic
    // takes (expectedExitReason), with the same guest address as the
    // interpreter's next address. A trap compares its kind and number too
    bool compareExit(const RowInfo& row, SemanticInterpreter::ExitReason reason,
                     const SemanticInterpreter& interpreter, uint64 exitReason,
                     uint64 exitPc)
    {
        const uint64 interpreterNext = interpreter.getNextAddress();
        const uint64 expected = expectedExitReason(row.m_text);
        if (expected == AMD64Lowering::EXIT_TRAP)
        {
            const uint64 kind = (exitReason >> AMD64Lowering::EXIT_TRAP_KIND_SHIFT) & 0xFF;
            const uint64 number = exitReason >> AMD64Lowering::EXIT_TRAP_NUMBER_SHIFT;
            if (((exitReason & 0xFF) != AMD64Lowering::EXIT_TRAP) ||
                (reason != SemanticInterpreter::EXIT_TRAP) ||
                (exitPc != interpreterNext) ||
                (kind != interpreter.getTrapKind()) ||
                (number != interpreter.getTrapNumber()))
            {
                fprintf(stderr, "TestSemanticLoweringArm64: %s: the trap (pc 0x%llx kind %llu "
                        "number %llu) differs from the interpreter (reason %d)\n",
                        row.m_text, (unsigned long long)exitPc,
                        (unsigned long long)kind, (unsigned long long)number, (int)reason);
                return false;
            }
            return true;
        }
        if (exitReason != expected)
        {
            fprintf(stderr, "TestSemanticLoweringArm64: %s: the native exit reason %llu, "
                    "expected %llu\n", row.m_text, (unsigned long long)exitReason,
                    (unsigned long long)expected);
            return false;
        }
        if (expected == REASON_LABEL)
        {
            // a fall through or a branch to a constant address: the label stub
            if (((reason != SemanticInterpreter::EXIT_FALL_THROUGH) &&
                 (reason != SemanticInterpreter::EXIT_BRANCH)) ||
                (exitPc != interpreterNext))
            {
                fprintf(stderr, "TestSemanticLoweringArm64: %s: the label exit (pc 0x%llx) "
                        "differs from the interpreter (reason %d, next 0x%llx)\n",
                        row.m_text, (unsigned long long)exitPc, (int)reason,
                        (unsigned long long)interpreterNext);
                return false;
            }
            return true;
        }
        // a register branch, call or return: the interpreter's branch
        if ((reason != SemanticInterpreter::EXIT_BRANCH) || (exitPc != interpreterNext))
        {
            fprintf(stderr, "TestSemanticLoweringArm64: %s: the indirect exit (pc 0x%llx) "
                    "differs from the interpreter (reason %d, next 0x%llx)\n", row.m_text,
                    (unsigned long long)exitPc, (int)reason,
                    (unsigned long long)interpreterNext);
            return false;
        }
        return true;
    }

    // Report a difference of a case
    bool reportCase(const RowInfo& row, const char* message)
    {
        fprintf(stderr, "TestSemanticLoweringArm64: %s: %s\n", row.m_text, message);
        return false;
    }

    // The guest and the target of the rows
    ArchitectureDescriptionPtr m_guest;
    ArchitectureDescriptionPtr m_target;
    // The rows that ran, failed and were skipped
    uint m_run;
    uint m_failed;
    uint m_skipped;
    // The passing and the skipped rows of each mnemonic
    uint m_passed[MNEMONIC_COUNT];
    uint m_rowSkipped[MNEMONIC_COUNT];
};

#else // !LOWERING_ARM64_NATIVE

class TestObjectTestSemanticLoweringArm64 : public cTestObject {
public:
    virtual void test()
    {
        cout << "arm64 rows: 0 run, 0 failed, 0 skipped (the native runs need an "
                "x86-64 Linux host)" << endl;
    }
    virtual cString getName() { return __FILE__; }
};

#endif

} // namespace

// Instance test object
TestObjectTestSemanticLoweringArm64 g_globalTestSemanticLoweringArm64;
