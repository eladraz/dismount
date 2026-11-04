#include "dismount/dismount.h"
/*
 * RiscvFeatures.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/data/list.h"
#include "xStl/except/trace.h"
#include "xStl/except/assert.h"
#include "xStl/exceptions.h"
#include "dismount/proc/riscv/RiscvFeatures.h"

namespace {

typedef RiscvFeatures F;

/*
 * The description of a feature: its LLVM name, its place in the RVA23
 * profiles, its support and the features it implies (LLVM's implications,
 * transitively, ended by FEATURES_COUNT).
 */
struct FeatureInfo {
    const char* m_name;
    uint m_profile;
    uint m_support;
    uint16 m_implied[8];
};

enum { END = F::FEATURES_COUNT };

// The support bits shortcuts
enum {
    SUPPORT_NONE = 0,
    // The decoder, the assembler and the comparison with LLVM (tests/riscv/
    // run_compat.py, fuzz_decode.py and fuzz_roundtrip.py)
    SUPPORT_FULL = F::SUPPORT_DISASSEMBLE | F::SUPPORT_ASSEMBLE |
                   F::SUPPORT_ENCODING_VERIFIED,
    // A feature without instructions: its CSR names are decoded and assembled
    SUPPORT_CSR_ONLY = F::SUPPORT_NO_INSTRUCTIONS | F::SUPPORT_DISASSEMBLE |
                       F::SUPPORT_ASSEMBLE | F::SUPPORT_ENCODING_VERIFIED
};

/*
 * The place of the features in the RVA23 profiles, from the ratified RVA23
 * profile specification:
 *
 * RVA23U64 mandatory: I, M, A, F, D, C, B, Zicsr, Zicntr, Zihpm, Ziccif,
 *   Ziccrse, Ziccamoa, Zicclsm, Za64rs, Zihintpause, Zic64b, Zicbom, Zicbop,
 *   Zicboz, Zfhmin, Zkt, V, Zvfhmin, Zvbb, Zvkt, Zihintntl, Zicond, Zimop,
 *   Zcmop, Zcb, Zfa, Zawrs, Supm
 * RVA23U64 optional: Zvkng, Zvksg (localized); Zabha, Zacas, Ziccamoc, Zvbc,
 *   Zama16b (development); Zfh, Zbc, Zicfilp, Zicfiss, Zvfh, Zfbfmin,
 *   Zvfbfmin, Zvfbfwma (expansion)
 * RVA23S64 mandatory (more than RVA23U64): Zifencei, Ss1p13, Svbare, Sv39,
 *   Svade, Ssccptr, Sstvecd, Sstvala, Sscounterenw, Svpbmt, Svinval, Svnapot,
 *   Sstc, Sscofpmf, Ssnpm, Ssu64xl, Sha (H, Ssstateen, Shcounterenw,
 *   Shvstvala, Shtvala, Shvstvecd, Shvsatpa, Shgatpa)
 * RVA23S64 optional: the RVA23U64 optional ones, and Sv48, Sv57, Zkr, Svadu,
 *   Sdtrig, Ssstrict, Svvptc, Sspm
 *
 * (Zvkng is Zvkned + Zvknhb + Zvkb + Zvkg + Zvkt, Zvksg is Zvksed + Zvksh +
 * Zvkb + Zvkg + Zvkt: their parts are optional in the same way.)
 */
enum {
    P_NONE = F::PROFILE_NONE,
    P_U64 = F::PROFILE_RVA23U64_MANDATORY,
    P_U64_OPT = F::PROFILE_RVA23U64_OPTIONAL,
    P_S64 = F::PROFILE_RVA23S64_MANDATORY,
    P_S64_OPT = F::PROFILE_RVA23S64_OPTIONAL
};

// The vector implications
#define V32X F::FEAT_ZVE32X, F::FEAT_ZICSR

const FeatureInfo gFeatures[F::FEATURES_COUNT] = {
    /* FEAT_I */          { "",                  P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_M */          { "m",                 P_U64,     SUPPORT_FULL, { F::FEAT_ZMMUL, END } },
    /* FEAT_ZMMUL */      { "zmmul",             P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZALRSC */     { "zalrsc",            P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZAAMO */      { "zaamo",             P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_F */          { "f",                 P_U64,     SUPPORT_FULL, { F::FEAT_ZICSR, END } },
    /* FEAT_D */          { "d",                 P_U64,     SUPPORT_FULL, { F::FEAT_F, F::FEAT_ZICSR, END } },
    /* FEAT_ZICSR */      { "zicsr",             P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZICNTR */     { "zicntr",            P_U64,     SUPPORT_FULL, { F::FEAT_ZICSR, END } },
    /* FEAT_ZIHPM */      { "zihpm",             P_U64,     SUPPORT_CSR_ONLY, { F::FEAT_ZICSR, END } },
    /* FEAT_ZIFENCEI */   { "zifencei",          P_S64,     SUPPORT_FULL, { END } },
    /* FEAT_ZIHINTPAUSE */{ "zihintpause",       P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZIHINTNTL */  { "zihintntl",         P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZICBOM */     { "zicbom",            P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZICBOP */     { "zicbop",            P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZICBOZ */     { "zicboz",            P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZICOND */     { "zicond",            P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZIMOP */      { "zimop",             P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZAWRS */      { "zawrs",             P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZACAS */      { "zacas",             P_U64_OPT, SUPPORT_FULL, { F::FEAT_ZAAMO, END } },
    /* FEAT_ZABHA */      { "zabha",             P_U64_OPT, SUPPORT_FULL, { F::FEAT_ZAAMO, END } },
    /* FEAT_ZICFILP */    { "experimental-zicfilp", P_U64_OPT, SUPPORT_FULL, { F::FEAT_ZICSR, END } },
    /* FEAT_ZICFISS */    { "experimental-zicfiss", P_U64_OPT, SUPPORT_FULL, { F::FEAT_ZICSR, F::FEAT_ZIMOP, END } },
    /* FEAT_ZBA */        { "zba",               P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZBB */        { "zbb",               P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZBS */        { "zbs",               P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZBC */        { "zbc",               P_U64_OPT, SUPPORT_FULL, { F::FEAT_ZBKC, END } },
    /* FEAT_ZBKB */       { "zbkb",              P_NONE,    SUPPORT_FULL, { END } },
    /* FEAT_ZBKC */       { "zbkc",              P_NONE,    SUPPORT_FULL, { END } },
    /* FEAT_ZBKX */       { "zbkx",              P_NONE,    SUPPORT_FULL, { END } },
    /* FEAT_ZKND */       { "zknd",              P_NONE,    SUPPORT_FULL, { END } },
    /* FEAT_ZKNE */       { "zkne",              P_NONE,    SUPPORT_FULL, { END } },
    /* FEAT_ZKNH */       { "zknh",              P_NONE,    SUPPORT_FULL, { END } },
    /* FEAT_ZKSED */      { "zksed",             P_NONE,    SUPPORT_FULL, { END } },
    /* FEAT_ZKSH */       { "zksh",              P_NONE,    SUPPORT_FULL, { END } },
    /* FEAT_ZKR */        { "zkr",               P_S64_OPT, SUPPORT_CSR_ONLY, { END } },
    /* FEAT_ZKT */        { "zkt",               P_U64,     SUPPORT_NONE | F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_ZFA */        { "zfa",               P_U64,     SUPPORT_FULL, { F::FEAT_F, F::FEAT_ZICSR, END } },
    /* FEAT_ZFH */        { "zfh",               P_U64_OPT, SUPPORT_FULL, { F::FEAT_F, F::FEAT_ZFHMIN, F::FEAT_ZICSR, END } },
    /* FEAT_ZFHMIN */     { "zfhmin",            P_U64,     SUPPORT_FULL, { F::FEAT_F, F::FEAT_ZICSR, END } },
    /* FEAT_ZFBFMIN */    { "zfbfmin",           P_U64_OPT, SUPPORT_FULL, { F::FEAT_F, F::FEAT_ZICSR, END } },
    /* FEAT_ZCA */        { "zca",               P_U64,     SUPPORT_FULL, { END } },
    /* FEAT_ZCF */        { "zcf",               P_NONE,    SUPPORT_FULL, { F::FEAT_F, F::FEAT_ZCA, F::FEAT_ZICSR, END } },
    /* FEAT_ZCD */        { "zcd",               P_U64,     SUPPORT_FULL, { F::FEAT_D, F::FEAT_F, F::FEAT_ZCA, F::FEAT_ZICSR, END } },
    /* FEAT_ZCB */        { "zcb",               P_U64,     SUPPORT_FULL, { F::FEAT_ZCA, END } },
    /* FEAT_ZCMOP */      { "zcmop",             P_U64,     SUPPORT_FULL, { F::FEAT_ZCA, END } },
    // The vector extension and its subsets: the vector table is filled by the
    // vector agent (RiscvEncodingTableVector.cpp), nothing is claimed here
    /* FEAT_ZVE32X */     { "zve32x",            P_U64,     SUPPORT_NONE, { F::FEAT_ZICSR, END } },
    /* FEAT_ZVE32F */     { "zve32f",            P_U64,     SUPPORT_NONE, { F::FEAT_F, F::FEAT_ZICSR, F::FEAT_ZVE32X, END } },
    /* FEAT_ZVE64X */     { "zve64x",            P_U64,     SUPPORT_NONE, { V32X, END } },
    /* FEAT_ZVE64F */     { "zve64f",            P_U64,     SUPPORT_NONE, { F::FEAT_F, F::FEAT_ZICSR, F::FEAT_ZVE32F, F::FEAT_ZVE32X, F::FEAT_ZVE64X, END } },
    /* FEAT_ZVE64D */     { "zve64d",            P_U64,     SUPPORT_NONE, { F::FEAT_D, F::FEAT_F, F::FEAT_ZICSR, F::FEAT_ZVE32F, F::FEAT_ZVE32X, F::FEAT_ZVE64F, F::FEAT_ZVE64X, END } },
    /* FEAT_V */          { "v",                 P_U64,     SUPPORT_NONE, { F::FEAT_D, F::FEAT_F, F::FEAT_ZICSR, F::FEAT_ZVE32F, F::FEAT_ZVE32X, F::FEAT_ZVE64D, F::FEAT_ZVE64F, F::FEAT_ZVE64X } },
    /* FEAT_ZVFH */       { "zvfh",              P_U64_OPT, SUPPORT_NONE, { F::FEAT_F, F::FEAT_ZFHMIN, F::FEAT_ZICSR, F::FEAT_ZVE32F, F::FEAT_ZVE32X, F::FEAT_ZVFHMIN, END } },
    /* FEAT_ZVFHMIN */    { "zvfhmin",           P_U64,     SUPPORT_NONE, { F::FEAT_F, F::FEAT_ZICSR, F::FEAT_ZVE32F, F::FEAT_ZVE32X, END } },
    /* FEAT_ZVFBFMIN */   { "zvfbfmin",          P_U64_OPT, SUPPORT_NONE, { F::FEAT_F, F::FEAT_ZICSR, F::FEAT_ZVE32F, F::FEAT_ZVE32X, END } },
    /* FEAT_ZVFBFWMA */   { "zvfbfwma",          P_U64_OPT, SUPPORT_NONE, { F::FEAT_F, F::FEAT_ZFBFMIN, F::FEAT_ZICSR, F::FEAT_ZVE32F, F::FEAT_ZVE32X, F::FEAT_ZVFBFMIN, END } },
    /* FEAT_ZVKB */       { "zvkb",              P_U64,     SUPPORT_NONE, { V32X, END } },
    /* FEAT_ZVBB */       { "zvbb",              P_U64,     SUPPORT_NONE, { F::FEAT_ZICSR, F::FEAT_ZVE32X, F::FEAT_ZVKB, END } },
    /* FEAT_ZVBC */       { "zvbc",              P_U64_OPT, SUPPORT_NONE, { F::FEAT_ZICSR, F::FEAT_ZVE32X, F::FEAT_ZVE64X, END } },
    /* FEAT_ZVKG */       { "zvkg",              P_U64_OPT, SUPPORT_NONE, { V32X, END } },
    /* FEAT_ZVKNED */     { "zvkned",            P_U64_OPT, SUPPORT_NONE, { V32X, END } },
    /* FEAT_ZVKNHA */     { "zvknha",            P_U64_OPT, SUPPORT_NONE, { V32X, END } },
    /* FEAT_ZVKNHB */     { "zvknhb",            P_U64_OPT, SUPPORT_NONE, { F::FEAT_ZICSR, F::FEAT_ZVE32X, F::FEAT_ZVE64X, F::FEAT_ZVKNHA, END } },
    /* FEAT_ZVKSED */     { "zvksed",            P_U64_OPT, SUPPORT_NONE, { V32X, END } },
    /* FEAT_ZVKSH */      { "zvksh",             P_U64_OPT, SUPPORT_NONE, { V32X, END } },
    /* FEAT_ZVKT */       { "zvkt",              P_U64,     SUPPORT_NONE | F::SUPPORT_NO_INSTRUCTIONS, { END } },
    // The properties of the memory system
    /* FEAT_ZICCIF */     { "ziccif",            P_U64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_ZICCRSE */    { "ziccrse",           P_U64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_ZICCAMOA */   { "ziccamoa",          P_U64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_ZICCAMOC */   { "ziccamoc",          P_U64_OPT, F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_ZICCLSM */    { "zicclsm",           P_U64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_ZA64RS */     { "za64rs",            P_U64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_ZIC64B */     { "zic64b",            P_U64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_ZAMA16B */    { "zama16b",           P_U64_OPT, F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SUPM */       { "supm",              P_U64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    // The privileged architecture
    /* FEAT_PRIVILEGED */ { "",                  P_S64,     SUPPORT_FULL, { END } },
    /* FEAT_SVINVAL */    { "svinval",           P_S64,     SUPPORT_FULL, { END } },
    /* FEAT_H */          { "h",                 P_S64,     SUPPORT_FULL, { END } },
    /* FEAT_SSTC */       { "sstc",              P_S64,     SUPPORT_CSR_ONLY, { END } },
    /* FEAT_SSCOFPMF */   { "sscofpmf",          P_S64,     SUPPORT_CSR_ONLY, { END } },
    /* FEAT_SSNPM */      { "ssnpm",             P_S64,     SUPPORT_CSR_ONLY, { END } },
    /* FEAT_SSSTATEEN */  { "ssstateen",         P_S64,     SUPPORT_CSR_ONLY, { END } },
    /* FEAT_SVPBMT */     { "svpbmt",            P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SVNAPOT */    { "svnapot",           P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SVADE */      { "svade",             P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SVBARE */     { "svbare",            P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SSCCPTR */    { "ssccptr",           P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SSTVECD */    { "sstvecd",           P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SSTVALA */    { "sstvala",           P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SSCOUNTERENW */{ "sscounterenw",     P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SSU64XL */    { "ssu64xl",           P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SHCOUNTERENW */{ "shcounterenw",     P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SHVSTVALA */  { "shvstvala",         P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SHTVALA */    { "shtvala",           P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SHVSTVECD */  { "shvstvecd",         P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SHVSATPA */   { "shvsatpa",          P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SHGATPA */    { "shgatpa",           P_S64,     F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SDTRIG */     { "sdtrig",            P_S64_OPT, F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SSSTRICT */   { "ssstrict",          P_S64_OPT, F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SVVPTC */     { "svvptc",            P_S64_OPT, F::SUPPORT_NO_INSTRUCTIONS, { END } },
    /* FEAT_SSPM */       { "sspm",              P_S64_OPT, F::SUPPORT_NO_INSTRUCTIONS, { END } }
};

/*
 * The shorthands and the groups of the ISA strings and the feature lists
 * (not features). Each one is a list of features ended by FEATURES_COUNT.
 */
struct Group {
    const char* m_name;
    uint16 m_features[12];
};

const Group gGroups[] = {
    { "a",     { F::FEAT_ZALRSC, F::FEAT_ZAAMO, END } },
    { "b",     { F::FEAT_ZBA, F::FEAT_ZBB, F::FEAT_ZBS, END } },
    { "g",     { F::FEAT_I, F::FEAT_M, F::FEAT_ZALRSC, F::FEAT_ZAAMO, F::FEAT_F, F::FEAT_D,
                 F::FEAT_ZICSR, F::FEAT_ZIFENCEI, END } },
    { "zk",    { F::FEAT_ZBKB, F::FEAT_ZBKC, F::FEAT_ZBKX, F::FEAT_ZKND, F::FEAT_ZKNE, F::FEAT_ZKNH,
                 F::FEAT_ZKR, F::FEAT_ZKT, END } },
    { "zkn",   { F::FEAT_ZBKB, F::FEAT_ZBKC, F::FEAT_ZBKX, F::FEAT_ZKND, F::FEAT_ZKNE, F::FEAT_ZKNH, END } },
    { "zks",   { F::FEAT_ZBKB, F::FEAT_ZBKC, F::FEAT_ZBKX, F::FEAT_ZKSED, F::FEAT_ZKSH, END } },
    { "zvkn",  { F::FEAT_ZVKB, F::FEAT_ZVKNED, F::FEAT_ZVKNHB, F::FEAT_ZVKT, END } },
    { "zvkng", { F::FEAT_ZVKB, F::FEAT_ZVKNED, F::FEAT_ZVKNHB, F::FEAT_ZVKT, F::FEAT_ZVKG, END } },
    { "zvknc", { F::FEAT_ZVKB, F::FEAT_ZVKNED, F::FEAT_ZVKNHB, F::FEAT_ZVKT, F::FEAT_ZVBC, END } },
    { "zvks",  { F::FEAT_ZVKB, F::FEAT_ZVKSED, F::FEAT_ZVKSH, F::FEAT_ZVKT, END } },
    { "zvksg", { F::FEAT_ZVKB, F::FEAT_ZVKSED, F::FEAT_ZVKSH, F::FEAT_ZVKT, F::FEAT_ZVKG, END } },
    { "zvksc", { F::FEAT_ZVKB, F::FEAT_ZVKSED, F::FEAT_ZVKSH, F::FEAT_ZVKT, F::FEAT_ZVBC, END } },
    { "sha",   { F::FEAT_H, F::FEAT_SHCOUNTERENW, F::FEAT_SHGATPA, F::FEAT_SHTVALA, F::FEAT_SHVSATPA,
                 F::FEAT_SHVSTVALA, F::FEAT_SHVSTVECD, F::FEAT_SSSTATEEN, END } }
};

// The names which LLVM knows and have nothing to decode here: accepted and
// ignored by parse(). "zvl128b".. (the minimum vector length), relax, ...
bool isIgnoredName(const cString& name)
{
    if (name == "relax")
        return true;
    if ((name.length() > 4) && (name.left(3) == "zvl") &&
        (name[name.length() - 1] == XSTL_CHAR('b')))
        return true;
    return false;
}

/*
 * The profiles. A profile is a list of the single letters and extensions of
 * its ISA string; the lists are the -target-feature lists of clang
 * (-march=<profile>), verified by tests/riscv/tools/check_profiles.py.
 */
struct Profile {
    const char* m_name;
    // The extension names, space separated, as parse() reads them
    const char* m_extensions;
};

const Profile gProfiles[] = {
    { "rvi20u32", "i" },
    { "rvi20u64", "i" },
    { "rva20u64", "i m a f d c zicsr zmmul ziccamoa ziccif zicclsm ziccrse zicntr" },
    { "rva22u64", "i m a f d c b zicsr zmmul za64rs zfhmin zic64b zicbom zicbop zicboz ziccamoa "
                  "ziccif zicclsm ziccrse zicntr zihintpause zihpm zkt" },
    { "rva22s64", "i m a f d c b zicsr zmmul za64rs zfhmin zic64b zicbom zicbop zicboz ziccamoa "
                  "ziccif zicclsm ziccrse zicntr zihintpause zihpm zkt zifencei ssccptr "
                  "sscounterenw sstvala sstvecd svade svbare svinval svpbmt" },
    { "rva23u64", "i m a f d c b v zicsr zmmul za64rs zawrs zcb zcmop zfa zfhmin zic64b zicbom zicbop "
                  "zicboz ziccamoa ziccif zicclsm ziccrse zicntr zicond zihintntl zihintpause zihpm "
                  "zimop zkt zvbb zvfhmin zvkb zvkt supm" },
    { "rva23s64", "i m a f d c b v zicsr zmmul za64rs zawrs zcb zcmop zfa zfhmin zic64b zicbom zicbop "
                  "zicboz ziccamoa ziccif zicclsm ziccrse zicntr zicond zihintntl zihintpause zihpm "
                  "zimop zkt zvbb zvfhmin zvkb zvkt supm zifencei sha shcounterenw shgatpa shtvala "
                  "shvsatpa shvstvala shvstvecd ssccptr sscofpmf sscounterenw ssnpm ssstateen sstc "
                  "sstvala sstvecd ssu64xl svade svbare svinval svnapot svpbmt" }
};

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
// RiscvFeatures

const char* RiscvFeatures::getName(Feature feature)
{
    CHECK(feature < FEATURES_COUNT);
    return gFeatures[feature].m_name;
}

bool RiscvFeatures::findFeature(const char* name, Feature& feature)
{
    cString text(name);
    text.makeLower();
    if (text.length() == 0)
        return false;
    for (uint i = 0; i < FEATURES_COUNT; i++)
    {
        if (gFeatures[i].m_name[0] == 0)
            continue;
        cString full(gFeatures[i].m_name);
        cString plain(full);
        if ((full.length() > 13) && (full.left(13) == "experimental-"))
            plain = full.part(13, full.length());
        if ((text == full) || (text == plain))
        {
            feature = (Feature)i;
            return true;
        }
    }
    return false;
}

RiscvFeatures::ProfileStatus RiscvFeatures::getProfileStatus(Feature feature)
{
    CHECK(feature < FEATURES_COUNT);
    return (ProfileStatus)gFeatures[feature].m_profile;
}

bool RiscvFeatures::isAvailable(Feature feature, RiscvInstructionSet::DisassemblerTypes type)
{
    CHECK(feature < FEATURES_COUNT);
    // Zcf (c.flw, c.fsw, c.flwsp, c.fswsp, c.jal's slot) is RV32 only
    if (feature == FEAT_ZCF)
        return type == RiscvInstructionSet::RISCV_RV32;
    return true;
}

uint RiscvFeatures::getSupport(Feature feature)
{
    CHECK(feature < FEATURES_COUNT);
    return gFeatures[feature].m_support;
}

//////////////////////////////////////////////////////////////////////////
// RiscvFeatureSet

RiscvFeatureSet::RiscvFeatureSet()
{
    for (uint i = 0; i < WORDS_COUNT; i++)
        m_bits[i] = 0;
    add(RiscvFeatures::FEAT_I);
    add(RiscvFeatures::FEAT_PRIVILEGED);
}

RiscvFeatureSet RiscvFeatureSet::all()
{
    RiscvFeatureSet result;
    for (uint i = 0; i < RiscvFeatures::FEATURES_COUNT; i++)
        result.add((RiscvFeatures::Feature)i);
    return result;
}

void RiscvFeatureSet::add(RiscvFeatures::Feature feature)
{
    CHECK(feature < RiscvFeatures::FEATURES_COUNT);
    m_bits[feature / 64] |= ((uint64)1 << (feature % 64));
}

void RiscvFeatureSet::remove(RiscvFeatures::Feature feature)
{
    CHECK(feature < RiscvFeatures::FEATURES_COUNT);
    // The base is on every processor
    if ((feature == RiscvFeatures::FEAT_I) ||
        (feature == RiscvFeatures::FEAT_PRIVILEGED))
        return;
    m_bits[feature / 64] &= ~((uint64)1 << (feature % 64));
}

bool RiscvFeatureSet::has(RiscvFeatures::Feature feature) const
{
    CHECK(feature < RiscvFeatures::FEATURES_COUNT);
    return ((m_bits[feature / 64] >> (feature % 64)) & 1) != 0;
}

bool RiscvFeatureSet::operator == (const RiscvFeatureSet& other) const
{
    for (uint i = 0; i < WORDS_COUNT; i++)
        if (m_bits[i] != other.m_bits[i])
            return false;
    return true;
}

namespace {

// Add a feature with the features it implies
void addWithImplied(RiscvFeatureSet& set, RiscvFeatures::Feature feature)
{
    set.add(feature);
    for (uint i = 0; i < 8; i++)
    {
        uint implied = gFeatures[feature].m_implied[i];
        if (implied >= RiscvFeatures::FEATURES_COUNT)
            break;
        set.add((RiscvFeatures::Feature)implied);
    }
}

// Remove a feature and the features which need it (LLVM disables the
// features which are implied by a disabled one: -zca disables zcb, zcd...)
void removeWithDependents(RiscvFeatureSet& set, RiscvFeatures::Feature feature)
{
    set.remove(feature);
    for (uint i = 0; i < RiscvFeatures::FEATURES_COUNT; i++)
    {
        for (uint j = 0; j < 8; j++)
        {
            uint implied = gFeatures[i].m_implied[j];
            if (implied >= RiscvFeatures::FEATURES_COUNT)
                break;
            if (implied == (uint)feature)
                set.remove((RiscvFeatures::Feature)i);
        }
    }
}

// Apply one name: a feature, a group, a shorthand. 'cShorthand' is set for
// the names whose compressed floating point forms depend on f / d (c, zce).
// Return false for an unknown name
bool applyName(RiscvFeatureSet& set, const cString& name, bool isRemove, bool& cShorthand)
{
    if (isIgnoredName(name))
        return true;
    if (name == "none")
    {
        set = RiscvFeatureSet();
        return true;
    }
    if (name == "all")
    {
        for (uint i = 0; i < RiscvFeatures::FEATURES_COUNT; i++)
        {
            if (isRemove)
                set.remove((RiscvFeatures::Feature)i);
            else
                set.add((RiscvFeatures::Feature)i);
        }
        return true;
    }
    if (name == "i")
        return true;
    // c is Zca (Zcf and Zcd if f / d, see the end of parse()); zce is Zca,
    // Zcb (and Zcf, Zcd)
    if ((name == "c") || (name == "zce"))
    {
        if (isRemove)
            removeWithDependents(set, RiscvFeatures::FEAT_ZCA);
        else
        {
            addWithImplied(set, RiscvFeatures::FEAT_ZCA);
            if (name == "zce")
            {
                addWithImplied(set, RiscvFeatures::FEAT_ZCB);
                addWithImplied(set, RiscvFeatures::FEAT_ZICSR);
            }
            cShorthand = true;
        }
        return true;
    }
    for (uint i = 0; i < sizeof(gGroups) / sizeof(gGroups[0]); i++)
    {
        if (!(name == gGroups[i].m_name))
            continue;
        for (uint j = 0; gGroups[i].m_features[j] < RiscvFeatures::FEATURES_COUNT; j++)
        {
            RiscvFeatures::Feature feature = (RiscvFeatures::Feature)gGroups[i].m_features[j];
            if (isRemove)
                removeWithDependents(set, feature);
            else
                addWithImplied(set, feature);
        }
        return true;
    }
    RiscvFeatures::Feature feature;
    if (!RiscvFeatures::findFeature(name.getASCIIstring().getBuffer(), feature))
        return false;
    if (isRemove)
        removeWithDependents(set, feature);
    else
        addWithImplied(set, feature);
    return true;
}

// Strip the version of an extension name of an ISA string ("zba1p0", "i2p1")
cString stripVersion(const cString& name)
{
    uint end = name.length();
    uint digits = 0;
    while ((end > 0) && (name[end - 1] >= XSTL_CHAR('0')) && (name[end - 1] <= XSTL_CHAR('9')))
    {
        end--;
        digits++;
    }
    if ((digits > 0) && (end > 1) && (name[end - 1] == XSTL_CHAR('p')) &&
        (name[end - 2] >= XSTL_CHAR('0')) && (name[end - 2] <= XSTL_CHAR('9')))
    {
        end--;
        while ((end > 0) && (name[end - 1] >= XSTL_CHAR('0')) && (name[end - 1] <= XSTL_CHAR('9')))
            end--;
    }
    return name.left(end);
}

} // anonymous namespace

RiscvFeatureSet RiscvFeatureSet::fromProfile(const char* profile)
{
    cString text(profile);
    text.makeLower();
    for (uint i = 0; i < sizeof(gProfiles) / sizeof(gProfiles[0]); i++)
    {
        if (!(text == gProfiles[i].m_name))
            continue;
        RiscvFeatureSet result;
        cString extensions(gProfiles[i].m_extensions);
        cList<cString> items = extensions.split(XSTL_STRING(" "));
        bool cShorthand = false;
        for (cList<cString>::iterator item = items.begin(); item != items.end(); item++)
        {
            bool ok = applyName(result, *item, false, cShorthand);
            CHECK(ok);
        }
        // c with d: zcd (the profiles are RV64: no zcf)
        if (result.has(RiscvFeatures::FEAT_ZCA) && result.has(RiscvFeatures::FEAT_D))
            result.add(RiscvFeatures::FEAT_ZCD);
        return result;
    }
    XSTL_THROW(cException, XSTL_STRING("unknown RISC-V profile"), EXCEPTION_FAILED);
}

bool RiscvFeatureSet::parse(const char* list)
{
    bool allKnown = true;
    bool cShorthand = false;
    cString text(list);
    text.makeLower();
    text.trim();

    // An ISA string: "rv64gcv_zba_zbb", "rva23u64_zvkng"
    bool isIsa = (text.length() > 2) && (text.left(2) == "rv") &&
                 (text.find(XSTL_STRING(",")) >= text.length());
    if (isIsa)
    {
        // An ISA string describes the whole target: it replaces the set
        *this = RiscvFeatureSet();
        uint pos = 2;
        // The XLEN
        if ((text.length() >= pos + 2) &&
            ((text.part(pos, pos + 2) == "32") || (text.part(pos, pos + 2) == "64")))
        {
            pos += 2;
        } else
        {
            // A profile: "rva23u64"
            uint end = pos;
            while ((end < text.length()) && (text[end] != XSTL_CHAR('_')))
                end++;
            XSTL_TRY
            {
                RiscvFeatureSet profile = fromProfile(
                    (cString("rv") + text.part(pos, end)).getASCIIstring().getBuffer());
                for (uint i = 0; i < RiscvFeatures::FEATURES_COUNT; i++)
                    if (profile.has((RiscvFeatures::Feature)i))
                        add((RiscvFeatures::Feature)i);
            }
            XSTL_CATCH_ALL
            {
                allKnown = false;
            }
            pos = end;
        }
        while (pos < text.length())
        {
            if (text[pos] == XSTL_CHAR('_'))
            {
                pos++;
                continue;
            }
            cString name;
            character c = text[pos];
            if ((c == XSTL_CHAR('s')) || (c == XSTL_CHAR('z')) || (c == XSTL_CHAR('x')))
            {
                uint end = pos;
                while ((end < text.length()) && (text[end] != XSTL_CHAR('_')))
                    end++;
                name = stripVersion(text.part(pos, end));
                pos = end;
            } else
            {
                // A single letter, then its version
                name = text.part(pos, pos + 1);
                pos++;
                while ((pos < text.length()) &&
                       (((text[pos] >= XSTL_CHAR('0')) && (text[pos] <= XSTL_CHAR('9'))) ||
                        ((text[pos] == XSTL_CHAR('p')) && (pos + 1 < text.length()) &&
                         (text[pos + 1] >= XSTL_CHAR('0')) && (text[pos + 1] <= XSTL_CHAR('9')))))
                    pos++;
            }
            if (!applyName(*this, name, false, cShorthand))
                allKnown = false;
        }
    } else
    {
        cList<cString> items = text.split(XSTL_STRING(","));
        for (cList<cString>::iterator item = items.begin(); item != items.end(); item++)
        {
            cString name(*item);
            name.trim();
            if (name.length() == 0)
                continue;
            bool isRemove = false;
            if ((name[0] == XSTL_CHAR('+')) || (name[0] == XSTL_CHAR('-')))
            {
                isRemove = (name[0] == XSTL_CHAR('-'));
                name = name.part(1, name.length());
                name.trim();
            }
            if (!applyName(*this, name, isRemove, cShorthand))
                allKnown = false;
        }
    }

    // As LLVM (updateCZceFeatureImplication) once the whole list is applied: c
    // and zce imply Zcf with f and Zcd with d
    if (cShorthand && has(RiscvFeatures::FEAT_ZCA))
    {
        if (has(RiscvFeatures::FEAT_F))
            add(RiscvFeatures::FEAT_ZCF);
        if (has(RiscvFeatures::FEAT_D))
            add(RiscvFeatures::FEAT_ZCD);
    }
    return allKnown;
}
