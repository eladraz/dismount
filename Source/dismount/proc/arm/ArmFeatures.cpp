#include "dismount/dismount.h"
/*
 * ArmFeatures.cpp
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
#include "dismount/proc/arm/ArmFeatures.h"

namespace {

/*
 * The description of a feature: its LLVM name and its support. The table is
 * indexed by ArmFeatures::Feature (checked by the first use). The names are
 * the LLVM target features: several Arm features share one when LLVM has a
 * single feature for them ("sha2" is FEAT_SHA1 and FEAT_SHA256), and the
 * AArch32 feature of a name is the one of the A64 feature (both are enabled
 * by ArmFeatureSet::parse).
 */
struct FeatureInfo {
    // NOTE: FEAT_DPB, FEAT_BTI and FEAT_RNG have no encoding of their own in
    // the base table (the hints bti / dc cvap / the rndr system register are
    // decoded without a feature, like LLVM does), so they are not marked
    const char* m_name;
    // The ArmFeatures::Support bits
    uint m_support;
};

// The support bits shortcuts
enum {
    SUPPORT_NONE = 0,
    // The decoder, the assembler and the comparison with LLVM
    SUPPORT_FULL = ArmFeatures::SUPPORT_DISASSEMBLE |
                   ArmFeatures::SUPPORT_ASSEMBLE |
                   ArmFeatures::SUPPORT_ENCODING_VERIFIED,
    // ... and every instruction is lifted. The lifted instructions of the
    // A64 base table (Arm64SemanticLift*.cpp) which pass the differential
    // tests against Unicorn and the Apple M4 (tests/arm/semantics) are
    // LIFTED_VERIFIED; the ones neither processor implements (CSSC, HBC, LSUI,
    // CMPBR, LSE128...) are LIFTED only: tests/TestArm64Semantics.cpp checks
    // them against the pseudocode of the Arm ARM
    SUPPORT_LIFTED = SUPPORT_FULL | ArmFeatures::SUPPORT_LIFT,
    SUPPORT_LIFTED_VERIFIED = SUPPORT_LIFTED | ArmFeatures::SUPPORT_LIFT_VERIFIED,
    // ... and some of the instructions are lifted
    SUPPORT_PARTIAL = SUPPORT_FULL | ArmFeatures::SUPPORT_PARTIAL_LIFT,
    SUPPORT_PARTIAL_VERIFIED = SUPPORT_PARTIAL | ArmFeatures::SUPPORT_LIFT_VERIFIED
};

const FeatureInfo gFeatures[ArmFeatures::FEATURES_COUNT] = {
    // --- AArch32
    // The A32 / T32 integer, VFP and Advanced SIMD tables (ArmSemanticTable.cpp,
    // ArmSemanticTableSimd.cpp) lift every encoding of a feature marked LIFT. The base
    // features are PARTIAL: the privileged instructions (cps, setend, srs, rfe, eret,
    // hvc, smc), the exception return forms, the coprocessors but the thread id
    // registers, swp and the pac / bti hints are not lifted. VERIFIED means lines of the
    // feature pass against Unicorn (the only AArch32 processor oracle); FP16, BF16 and
    // i8mm are not in Unicorn 2.1.4 and are checked by the unit rows of
    // tests/TestArmAArch32Semantics.cpp (an independent Python model) only.
    /* FEAT_A32_BASE */      { "",                SUPPORT_PARTIAL_VERIFIED },
    /* FEAT_T32_BASE */      { "",                SUPPORT_PARTIAL_VERIFIED },
    /* FEAT_V5TE */          { "v5te",            SUPPORT_PARTIAL_VERIFIED },
    /* FEAT_V6 */            { "v6",              SUPPORT_LIFTED_VERIFIED },
    /* FEAT_V6K */           { "v6k",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_V6T2 */          { "v6t2",            SUPPORT_PARTIAL_VERIFIED },
    /* FEAT_V7 */            { "v7",              SUPPORT_LIFTED_VERIFIED },
    /* FEAT_DIV */           { "hwdiv",           SUPPORT_LIFTED_VERIFIED },
    /* FEAT_V8_AA32 */       { "v8",              SUPPORT_PARTIAL_VERIFIED },
    /* FEAT_CRC32_AA32 */    { "crc",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_AES_AA32 */      { "aes",             SUPPORT_LIFTED },
    /* FEAT_SHA1_AA32 */     { "sha2",            SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SHA256_AA32 */   { "sha2",            SUPPORT_LIFTED_VERIFIED },
    /* FEAT_VFP2 */          { "vfp2",            SUPPORT_LIFTED },
    /* FEAT_VFP3 */          { "vfp3",            SUPPORT_LIFTED },
    /* FEAT_VFP4 */          { "vfp4",            SUPPORT_LIFTED },
    /* FEAT_NEON */          { "neon",            SUPPORT_LIFTED_VERIFIED },
    // (the NEON vfma.f32 / vfms.f32 are tagged FEAT_VFP4: LLVM needs neon and vfp4)
    /* FEAT_NEON_VFP4 */     { "vfp4",            SUPPORT_NONE },
    /* FEAT_RDM_AA32 */      { "v8.1a",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_FP16_AA32 */     { "fullfp16",        SUPPORT_LIFTED },
    /* FEAT_DOTPROD_AA32 */  { "dotprod",         SUPPORT_LIFTED },
    /* FEAT_I8MM_AA32 */     { "i8mm",            SUPPORT_LIFTED },
    /* FEAT_BF16_AA32 */     { "bf16",            SUPPORT_LIFTED },
    /* FEAT_FCMA_AA32 */     { "v8.3a",       SUPPORT_LIFTED },
    /* FEAT_JSCVT_AA32 */    { "v8.3a",          SUPPORT_LIFTED },
    /* FEAT_FHM_AA32 */      { "fp16fml",         SUPPORT_LIFTED },

    // --- AArch64
    // The A64 base table (data processing, branches, loads and stores, system):
    // every instruction is lifted but the ones that need state the IR doesn't
    // have (the privileged and exception level instructions eret, drps, dcps,
    // hvc, smc, at, tlbi, sys, sysl, the debug and trace hints brb, trcit...). The
    // scalar floating point and Advanced SIMD data processing of this feature is
    // lifted by the table of its area (Arm64SemanticTableSimd.cpp)
    /* FEAT_A64_BASE */      { "",                SUPPORT_PARTIAL_VERIFIED },
    // The cryptographic extensions (Arm64SemanticLiftCrypto.cpp) and the Advanced SIMD
    // and floating point extensions below are lifted and pass the differential tests
    // against Unicorn and the Apple M4 (the M4 has no SM3 / SM4: Unicorn runs them)
    /* FEAT_AES */           { "aes",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SHA1 */          { "sha2",            SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SHA256 */        { "sha2",            SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SHA512 */        { "sha3",            SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SHA3 */          { "sha3",            SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SM3 */           { "sm4",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SM4 */           { "sm4",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_CRC32 */         { "crc",             SUPPORT_LIFTED_VERIFIED },
    // LSE: cas, casp, swp, ld<op>, st<op>. Unicorn swaps ldsmax / ldsmin (the host,
    // which is the arbiter, agrees with dismount, see oracle_bugs.txt)
    /* FEAT_LSE */           { "lse",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_RDM */           { "rdm",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_FP16 */          { "fullfp16",        SUPPORT_LIFTED_VERIFIED },
    /* FEAT_FHM */           { "fp16fml",         SUPPORT_LIFTED_VERIFIED },
    /* FEAT_DOTPROD */       { "dotprod",         SUPPORT_LIFTED_VERIFIED },
    /* FEAT_DPB */           { "ccpp",            SUPPORT_NONE },
    // PAuth: xpaci, xpacd and xpaclri strip the code exactly (47 bit addresses of
    // the user mode, checked against the M4); the hint space forms (paciasp,
    // autiasp...) are lifted to a helper (ARM_HELPER_INSTRUCTION); the keys and the
    // QARMA algorithm are not modeled, so pac*, aut*, the branches and returns with
    // authentication and ldraa / ldrab are not lifted
    /* FEAT_PAUTH */         { "pauth",           SUPPORT_PARTIAL_VERIFIED },
    /* FEAT_FCMA */          { "complxnum",       SUPPORT_LIFTED_VERIFIED },
    /* FEAT_JSCVT */         { "jsconv",          SUPPORT_LIFTED_VERIFIED },
    /* FEAT_LRCPC */         { "rcpc",            SUPPORT_LIFTED_VERIFIED },
    /* FEAT_LRCPC2 */        { "rcpc-immo",       SUPPORT_LIFTED_VERIFIED },
    /* FEAT_FLAGM */         { "flagm",           SUPPORT_LIFTED_VERIFIED },
    /* FEAT_FLAGM2 */        { "altnzcv",         SUPPORT_LIFTED_VERIFIED },
    /* FEAT_FRINTTS */       { "fptoint",         SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SB */            { "sb",              SUPPORT_LIFTED_VERIFIED },
    /* FEAT_BTI */           { "bti",             SUPPORT_NONE },
    // MTE: the allocation tags are not part of the memory of the IR: not lifted
    /* FEAT_MTE */           { "mte",             SUPPORT_FULL },
    /* FEAT_RNG */           { "rand",            SUPPORT_NONE },
    // SPECRES: cfp, dvp, cpp, cosp restrict the speculation: no architectural
    // effect (neither oracle implements them: TestArm64Semantics.cpp)
    /* FEAT_SPECRES */       { "predres",         SUPPORT_LIFTED },
    /* FEAT_I8MM */          { "i8mm",            SUPPORT_LIFTED_VERIFIED },
    // BF16: the standard behaviors (FPCR.EBF = 0; the M4 has no FEAT_EBF16); Unicorn has no BF16
    // and the M4 is the oracle
    /* FEAT_BF16 */          { "bf16",            SUPPORT_LIFTED_VERIFIED },
    // LS64: ld64b and st64b are eight loads / stores (neither oracle runs them);
    // st64bv and st64bv0 (the status result of the device write) are not lifted
    /* FEAT_LS64 */          { "ls64",            SUPPORT_PARTIAL },
    /* FEAT_WFXT */          { "wfxt",            SUPPORT_LIFTED },
    // MOPS: the memory copy and set instructions (the prologue / main / epilogue
    // split is implementation defined and no oracle runs them): not lifted
    /* FEAT_MOPS */          { "mops",            SUPPORT_FULL },
    /* FEAT_HBC */           { "hbc",             SUPPORT_LIFTED },
    /* FEAT_CSSC */          { "cssc",            SUPPORT_LIFTED },
    /* FEAT_LRCPC3 */        { "rcpc3",           SUPPORT_LIFTED },
    /* FEAT_LSE128 */        { "lse128",          SUPPORT_LIFTED },
    // D128: the 128 bit system registers: not lifted
    /* FEAT_SYSREG128 */     { "d128",            SUPPORT_FULL },
    // GCS: the guarded control stack: not lifted
    /* FEAT_GCS */           { "gcs",             SUPPORT_FULL },
    // CPA: the checked pointer arithmetic depends on the system configuration: not lifted
    /* FEAT_CPA */           { "cpa",             SUPPORT_FULL },
    // FP8: fscale is lifted (neither oracle runs it: Arm64SimdSemanticRowsFloat.inc checks it).
    // The conversions from and to the FP8 formats (f1cvtl, bf1cvtl, fcvtn of two sources...)
    // and the FP8 products (FP8FMA, FP8DOT2, FP8DOT4, F8F16MM, F8F32MM) depend on the FPMR
    // register (formats, scaling, overflow saturation), which the description doesn't model:
    // not lifted
    /* FEAT_FP8 */           { "fp8",             SUPPORT_PARTIAL },
    /* FEAT_FP8FMA */        { "fp8fma",          SUPPORT_FULL },
    /* FEAT_FP8DOT4 */       { "fp8dot4",         SUPPORT_FULL },
    /* FEAT_FP8DOT2 */       { "fp8dot2",         SUPPORT_FULL },
    // FAMINMAX, LUT: lifted; neither oracle runs them, Arm64SimdSemanticRowsFloat.inc checks them. The SVE forms of LUTI2 / LUTI4 are not lifted
    /* FEAT_FAMINMAX */      { "faminmax",        SUPPORT_LIFTED },
    /* FEAT_LUT */           { "lut",             SUPPORT_PARTIAL },

    // --- SVE
    // The SVE lifters (Arm64SemanticLiftSve*.cpp) lift every instruction of the features below, at the
    // vector length of the Arm64Architecture. LIFTED_VERIFIED: the instructions pass the differential tests
    // of tests/arm/semantics against Unicorn (SVE) and the Apple M4 (SVE2, SVE2.1 and the SME2 forms in the
    // streaming mode, at 512 bits); the lifted instructions neither processor runs are checked by
    // tests/TestArm64SemanticsSve.cpp against an independent model of the Arm ARM pseudocode only (F32MM,
    // F64MM, the crypto extensions, B16B16, BFSCALE and the bf16 / fp16 forms of the later extensions)
    /* FEAT_SVE */           { "sve",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_F32MM */         { "f32mm",           SUPPORT_LIFTED },
    /* FEAT_F64MM */         { "f64mm",           SUPPORT_LIFTED },
    /* FEAT_SVE2 */          { "sve2",            SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SVE_AES */       { "sve2-aes",        SUPPORT_LIFTED },
    /* FEAT_SVE_SHA3 */      { "sve2-sha3",       SUPPORT_LIFTED },
    /* FEAT_SVE_SM4 */       { "sve2-sm4",        SUPPORT_LIFTED },
    /* FEAT_SVE_BITPERM */   { "sve2-bitperm",    SUPPORT_LIFTED },
    /* FEAT_SVE2P1 */        { "sve2p1",          SUPPORT_LIFTED_VERIFIED },
    /* FEAT_SVE_B16B16 */    { "sve-b16b16",      SUPPORT_LIFTED },

    // --- SME
    /* FEAT_SME */           { "sme",             SUPPORT_FULL },
    /* FEAT_SME_F64F64 */    { "sme-f64f64",      SUPPORT_FULL },
    /* FEAT_SME_I16I64 */    { "sme-i16i64",      SUPPORT_FULL },
    /* FEAT_SME_F16F16 */    { "sme-f16f16",      SUPPORT_FULL },
    /* FEAT_SME2 */          { "sme2",            SUPPORT_FULL },
    /* FEAT_SME2P1 */        { "sme2p1",          SUPPORT_FULL },
    /* FEAT_SME_LUTV2 */     { "sme-lutv2",       SUPPORT_FULL },

    // --- Added with the base table
    /* FEAT_XS */            { "xs",              SUPPORT_LIFTED_VERIFIED },
    // TME: the transactional memory: not lifted
    /* FEAT_TME */           { "tme",             SUPPORT_FULL },
    /* FEAT_LSUI */          { "lsui",            SUPPORT_LIFTED },
    /* FEAT_CMPBR */         { "cmpbr",           SUPPORT_LIFTED },
    /* FEAT_LOR */           { "lor",             SUPPORT_LIFTED_VERIFIED },
    // THE: the read check write atomics (rcw*): not lifted
    /* FEAT_THE */           { "the",             SUPPORT_FULL },
    // LSFE: the floating point atomics: not lifted
    /* FEAT_LSFE */          { "lsfe",            SUPPORT_FULL },
    // PAuth_LR: see FEAT_PAUTH: not lifted
    /* FEAT_PAUTH_LR */      { "pauth-lr",        SUPPORT_FULL },
    /* FEAT_SPE */           { "spe",             SUPPORT_LIFTED },
    /* FEAT_TRF */           { "tracev8.4",       SUPPORT_LIFTED_VERIFIED },
    /* FEAT_RAS */           { "ras",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_PCDPHINT */      { "pcdphint",        SUPPORT_LIFTED },
    // POE2: the permission overlays: not lifted
    /* FEAT_POE2 */          { "poe2",            SUPPORT_FULL },
    // TEV: the transactional exit: not lifted
    /* FEAT_TEV */           { "tev",             SUPPORT_FULL },
    // F8F32MM, F8F16MM: the FP8 matrix multiplies (see FEAT_FP8): not lifted
    /* FEAT_F8F32MM */       { "f8f32mm",         SUPPORT_FULL },
    /* FEAT_F8F16MM */       { "f8f16mm",         SUPPORT_FULL },
    // FPRCVT: the conversions between the integer and floating point scalar SIMD registers of
    // different sizes; neither oracle runs them (Arm64SimdSemanticRowsFloat.inc)
    /* FEAT_FPRCVT */        { "fprcvt",          SUPPORT_LIFTED },

    // --- AArch32 features added with the A32 / T32 tables
    /* FEAT_V5T */           { "v5t",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_V6M */           { "v6m",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_MP */            { "mp",              SUPPORT_LIFTED_VERIFIED },
    /* FEAT_DIV_ARM */       { "hwdiv-arm",       SUPPORT_LIFTED_VERIFIED },
    /* FEAT_VIRT */          { "virtualization",  SUPPORT_PARTIAL },
    /* FEAT_TRUSTZONE */     { "trustzone",       SUPPORT_FULL },
    /* FEAT_SB_AA32 */       { "sb",              SUPPORT_LIFTED },
    /* FEAT_RAS_AA32 */      { "ras",             SUPPORT_LIFTED_VERIFIED },
    /* FEAT_PAN_AA32 */      { "v8.1a",           SUPPORT_FULL },
    /* FEAT_V8_4A_AA32 */    { "v8.4a",           SUPPORT_LIFTED },
    /* FEAT_FPARMV8 */       { "fp-armv8",        SUPPORT_LIFTED_VERIFIED },
    /* FEAT_FP16_CVT */      { "fp16",            SUPPORT_LIFTED_VERIFIED },

    // --- More SVE
    // Not lifted: SVE2.2 sabal / uabal, SVE2.3 addqp / addsubp and the unpredicated scvtf / ucvtf of bytes,
    // sve-aes2 pmull / pmlal (multiple vectors); the semantics of these are not known well enough and no
    // processor runs them. The rest is lifted and checked by the unit rows only
    /* FEAT_SVE2P2 */        { "sve2p2",          SUPPORT_PARTIAL },
    /* FEAT_SVE2P3 */        { "sve2p3",          SUPPORT_PARTIAL },
    /* FEAT_SVE_AES2 */      { "sve-aes2",        SUPPORT_PARTIAL },
    /* FEAT_SVE_BFSCALE */   { "sve-bfscale",     SUPPORT_LIFTED },

    // --- More SME. sme-fa64, ssve-aes, ssve-bitperm and ssve-fexpa have no encodings of their
    // own (they allow the full SVE / Advanced SIMD and some SVE extensions in the streaming
    // mode); ssve-fp8fma / dot2 / dot4 are the alternative of the SVE FP8 instructions
    /* FEAT_SME_B16B16 */    { "sme-b16b16",      SUPPORT_FULL },
    /* FEAT_SME_F8F16 */     { "sme-f8f16",       SUPPORT_FULL },
    /* FEAT_SME_F8F32 */     { "sme-f8f32",       SUPPORT_FULL },
    /* FEAT_SME_MOP4 */      { "sme-mop4",        SUPPORT_FULL },
    /* FEAT_SME_TMOP */      { "sme-tmop",        SUPPORT_FULL },
    /* FEAT_SME2P2 */        { "sme2p2",          SUPPORT_FULL },
    /* FEAT_SME2P3 */        { "sme2p3",          SUPPORT_FULL },
    /* FEAT_SME_FA64 */      { "sme-fa64",        SUPPORT_NONE },
    /* FEAT_SSVE_FP8FMA */   { "ssve-fp8fma",     SUPPORT_FULL },
    /* FEAT_SSVE_FP8DOT2 */  { "ssve-fp8dot2",    SUPPORT_FULL },
    /* FEAT_SSVE_FP8DOT4 */  { "ssve-fp8dot4",    SUPPORT_FULL },
    /* FEAT_SSVE_AES */      { "ssve-aes",        SUPPORT_NONE },
    /* FEAT_SSVE_BITPERM */  { "ssve-bitperm",    SUPPORT_NONE },
    /* FEAT_SSVE_FEXPA */    { "ssve-fexpa",      SUPPORT_NONE }
};

/*
 * The features which a feature of the LLVM list brings with it ("+sve2" is
 * "+sve2,+sve"). LLVM's implications, the ones which matter to the encodings.
 */
struct Implication {
    ArmFeatures::Feature m_feature;
    ArmFeatures::Feature m_implied;
};

const Implication gImplications[] = {
    { ArmFeatures::FEAT_LRCPC2,       ArmFeatures::FEAT_LRCPC },
    { ArmFeatures::FEAT_LRCPC3,       ArmFeatures::FEAT_LRCPC2 },
    { ArmFeatures::FEAT_LSE128,       ArmFeatures::FEAT_LSE },
    { ArmFeatures::FEAT_FLAGM2,       ArmFeatures::FEAT_FLAGM },
    { ArmFeatures::FEAT_FP8FMA,       ArmFeatures::FEAT_FP8 },
    { ArmFeatures::FEAT_FP8DOT4,      ArmFeatures::FEAT_FP8FMA },
    { ArmFeatures::FEAT_FP8DOT2,      ArmFeatures::FEAT_FP8DOT4 },
    { ArmFeatures::FEAT_F32MM,        ArmFeatures::FEAT_SVE },
    { ArmFeatures::FEAT_F64MM,        ArmFeatures::FEAT_SVE },
    { ArmFeatures::FEAT_SVE2,         ArmFeatures::FEAT_SVE },
    { ArmFeatures::FEAT_SVE_AES,      ArmFeatures::FEAT_SVE2 },
    { ArmFeatures::FEAT_SVE_SHA3,     ArmFeatures::FEAT_SVE2 },
    { ArmFeatures::FEAT_SVE_SM4,      ArmFeatures::FEAT_SVE2 },
    { ArmFeatures::FEAT_SVE_BITPERM,  ArmFeatures::FEAT_SVE2 },
    { ArmFeatures::FEAT_SVE2P1,       ArmFeatures::FEAT_SVE2 },
    { ArmFeatures::FEAT_SVE_B16B16,   ArmFeatures::FEAT_SVE },
    { ArmFeatures::FEAT_SVE2P2,       ArmFeatures::FEAT_SVE2P1 },
    { ArmFeatures::FEAT_SVE2P3,       ArmFeatures::FEAT_SVE2P2 },
    { ArmFeatures::FEAT_SVE_AES2,     ArmFeatures::FEAT_SVE2 },
    { ArmFeatures::FEAT_SVE_BFSCALE,  ArmFeatures::FEAT_SVE },
    { ArmFeatures::FEAT_F8F32MM,      ArmFeatures::FEAT_FP8 },
    { ArmFeatures::FEAT_F8F16MM,      ArmFeatures::FEAT_FP8 },
    { ArmFeatures::FEAT_SME_F64F64,   ArmFeatures::FEAT_SME },
    { ArmFeatures::FEAT_SME_I16I64,   ArmFeatures::FEAT_SME },
    { ArmFeatures::FEAT_SME2,         ArmFeatures::FEAT_SME },
    { ArmFeatures::FEAT_SME2P1,       ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SME_F16F16,   ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SME_LUTV2,    ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SME_B16B16,   ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SME_B16B16,   ArmFeatures::FEAT_SVE_B16B16 },
    { ArmFeatures::FEAT_SME_F8F32,    ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SME_F8F32,    ArmFeatures::FEAT_FP8 },
    { ArmFeatures::FEAT_SME_F8F16,    ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SME_F8F16,    ArmFeatures::FEAT_FP8 },
    { ArmFeatures::FEAT_SME_MOP4,     ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SME_TMOP,     ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SME2P2,       ArmFeatures::FEAT_SME2P1 },
    { ArmFeatures::FEAT_SME2P3,       ArmFeatures::FEAT_SME2P2 },
    { ArmFeatures::FEAT_SME_FA64,     ArmFeatures::FEAT_SME },
    { ArmFeatures::FEAT_SSVE_FP8FMA,  ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SSVE_FP8FMA,  ArmFeatures::FEAT_FP8 },
    { ArmFeatures::FEAT_SSVE_FP8DOT4, ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SSVE_FP8DOT4, ArmFeatures::FEAT_FP8 },
    { ArmFeatures::FEAT_SSVE_FP8DOT2, ArmFeatures::FEAT_SME2 },
    { ArmFeatures::FEAT_SSVE_FP8DOT2, ArmFeatures::FEAT_FP8 },
    { ArmFeatures::FEAT_SSVE_AES,     ArmFeatures::FEAT_SME },
    { ArmFeatures::FEAT_SSVE_BITPERM, ArmFeatures::FEAT_SME },
    { ArmFeatures::FEAT_SSVE_FEXPA,   ArmFeatures::FEAT_SME },
    { ArmFeatures::FEAT_VFP3,         ArmFeatures::FEAT_VFP2 },
    { ArmFeatures::FEAT_VFP4,         ArmFeatures::FEAT_VFP3 },
    { ArmFeatures::FEAT_NEON,         ArmFeatures::FEAT_VFP3 },
    { ArmFeatures::FEAT_NEON_VFP4,    ArmFeatures::FEAT_NEON },
    // The AArch32 architecture versions
    { ArmFeatures::FEAT_V5TE,         ArmFeatures::FEAT_V5T },
    { ArmFeatures::FEAT_V6,           ArmFeatures::FEAT_V5TE },
    { ArmFeatures::FEAT_V6K,          ArmFeatures::FEAT_V6 },
    { ArmFeatures::FEAT_V6M,          ArmFeatures::FEAT_V6 },
    { ArmFeatures::FEAT_V6T2,         ArmFeatures::FEAT_V6K },
    { ArmFeatures::FEAT_V6T2,         ArmFeatures::FEAT_V6M },
    { ArmFeatures::FEAT_V7,           ArmFeatures::FEAT_V6T2 },
    { ArmFeatures::FEAT_MP,           ArmFeatures::FEAT_V7 },
    { ArmFeatures::FEAT_V8_AA32,      ArmFeatures::FEAT_V7 },
    { ArmFeatures::FEAT_PAN_AA32,     ArmFeatures::FEAT_V8_AA32 },
    { ArmFeatures::FEAT_V8_4A_AA32,   ArmFeatures::FEAT_PAN_AA32 },
    { ArmFeatures::FEAT_FPARMV8,      ArmFeatures::FEAT_VFP4 },
    // The extensions that live in the coprocessor space of A32 (cp 8 - 15 of
    // the unconditional space: vsel, vsdot, vcmla...) are ARMv8 ones: the
    // pre ARMv8 coprocessor instructions are not there
    { ArmFeatures::FEAT_FPARMV8,      ArmFeatures::FEAT_V8_AA32 },
    { ArmFeatures::FEAT_DOTPROD_AA32, ArmFeatures::FEAT_V8_AA32 },
    { ArmFeatures::FEAT_I8MM_AA32,    ArmFeatures::FEAT_V8_AA32 },
    { ArmFeatures::FEAT_BF16_AA32,    ArmFeatures::FEAT_V8_AA32 },
    { ArmFeatures::FEAT_FCMA_AA32,    ArmFeatures::FEAT_V8_AA32 },
    { ArmFeatures::FEAT_FHM_AA32,     ArmFeatures::FEAT_V8_AA32 },
    { ArmFeatures::FEAT_FP16_CVT,     ArmFeatures::FEAT_VFP2 },
    { ArmFeatures::FEAT_FP16_AA32,    ArmFeatures::FEAT_FPARMV8 },
    { ArmFeatures::FEAT_FHM_AA32,     ArmFeatures::FEAT_FP16_AA32 },
    { ArmFeatures::FEAT_RDM_AA32,     ArmFeatures::FEAT_NEON },
    { ArmFeatures::FEAT_DOTPROD_AA32, ArmFeatures::FEAT_NEON },
    { ArmFeatures::FEAT_I8MM_AA32,    ArmFeatures::FEAT_NEON },
    { ArmFeatures::FEAT_BF16_AA32,    ArmFeatures::FEAT_NEON },
    { ArmFeatures::FEAT_FCMA_AA32,    ArmFeatures::FEAT_FPARMV8 },
    { ArmFeatures::FEAT_JSCVT_AA32,   ArmFeatures::FEAT_FPARMV8 },
    { ArmFeatures::FEAT_AES_AA32,     ArmFeatures::FEAT_NEON },
    { ArmFeatures::FEAT_SHA1_AA32,    ArmFeatures::FEAT_NEON },
    { ArmFeatures::FEAT_SHA256_AA32,  ArmFeatures::FEAT_NEON }
};

} // anonymous namespace

//////////////////////////////////////////////////////////////////////////
// ArmFeatures

const char* ArmFeatures::getName(Feature feature)
{
    CHECK(feature < FEATURES_COUNT);
    return gFeatures[feature].m_name;
}

bool ArmFeatures::findFeature(const char* name, Feature& feature)
{
    cString text(name);
    text.makeLower();
    if (text.length() == 0)
        return false;
    // A name which is shared by an A64 and an AArch32 feature finds the A64
    // one (ArmFeatureSet::parse sets all of them)
    for (uint pass = 0; pass < 2; pass++)
        for (uint i = 0; i < FEATURES_COUNT; i++)
            if (((i >= FEAT_A64_BASE) == (pass == 0)) &&
                (text == gFeatures[i].m_name))
            {
                feature = (Feature)i;
                return true;
            }
    return false;
}

ArmInstructionSet::DisassemblerTypes ArmFeatures::getInstructionSet(Feature feature)
{
    CHECK(feature < FEATURES_COUNT);
    if ((feature == FEAT_T32_BASE) || (feature == FEAT_V6T2))
        return ArmInstructionSet::ARM_T32;
    if ((feature < FEAT_A64_BASE) || ((feature >= FEAT_V5T) && (feature <= FEAT_FP16_CVT)))
        return ArmInstructionSet::ARM_A32;
    return ArmInstructionSet::ARM_A64;
}

uint ArmFeatures::getSupport(Feature feature)
{
    CHECK(feature < FEATURES_COUNT);
    return gFeatures[feature].m_support;
}

//////////////////////////////////////////////////////////////////////////
// ArmFeatureSet

ArmFeatureSet::ArmFeatureSet()
{
    for (uint i = 0; i < WORDS_COUNT; i++)
        m_bits[i] = 0;
    add(ArmFeatures::FEAT_A32_BASE);
    add(ArmFeatures::FEAT_T32_BASE);
    add(ArmFeatures::FEAT_A64_BASE);
}

ArmFeatureSet ArmFeatureSet::all()
{
    ArmFeatureSet result;
    for (uint i = 0; i < ArmFeatures::FEATURES_COUNT; i++)
        result.add((ArmFeatures::Feature)i);
    return result;
}

ArmFeatureSet ArmFeatureSet::fromArchitecture(const char* version)
{
    // "armv<major>[.<minor>]-a"
    cString text(version);
    text.makeLower();

    // The older AArch32 architectures have no profile: armv4t, armv5t, armv5te,
    // armv6, armv6k, armv6t2
    static const struct { const char* m_name; uint m_level; } older[] = {
        { "armv4t", 0 }, { "armv5t", 1 }, { "armv5te", 2 }, { "armv6", 3 }, { "armv6k", 4 },
        { "armv6t2", 5 }
    };
    for (uint i = 0; i < sizeof(older) / sizeof(older[0]); i++)
        if (text == older[i].m_name)
        {
            ArmFeatureSet old;
            if (older[i].m_level >= 1)
                old.add(ArmFeatures::FEAT_V5T);
            if (older[i].m_level >= 2)
                old.add(ArmFeatures::FEAT_V5TE);
            if (older[i].m_level >= 3)
                old.add(ArmFeatures::FEAT_V6);
            if (older[i].m_level >= 4)
                old.add(ArmFeatures::FEAT_V6K);
            if (older[i].m_level >= 5)
            {
                old.add(ArmFeatures::FEAT_V6M);
                old.add(ArmFeatures::FEAT_V6T2);
            }
            return old;
        }

    uint major = 0;
    uint minor = 0;
    bool valid = (text.length() >= 7) && (text.left(4) == "armv") &&
                 (text.right(2) == "-a");
    uint position = 4;
    if (valid)
    {
        valid = (text[position] >= XSTL_CHAR('7')) && (text[position] <= XSTL_CHAR('9'));
        major = valid ? (text[position] - XSTL_CHAR('0')) : 0;
        position++;
    }
    if (valid && (position + 2 < text.length()) && (text[position] == XSTL_CHAR('.')))
    {
        minor = text[position + 1] - XSTL_CHAR('0');
        valid = (text[position + 1] >= XSTL_CHAR('0')) &&
                (text[position + 1] <= XSTL_CHAR('9')) &&
                (position + 2 + 2 == text.length());
        position += 2;
    } else if (valid)
    {
        valid = (position + 2 == text.length());
    }
    if (!valid || ((major == 7) && (minor != 0)) ||
        ((major == 8) && (minor > 9)) || ((major == 9) && (minor > 5)))
        XSTL_THROW(cException, XSTL_STRING("unknown architecture version"), EXCEPTION_FAILED);

    ArmFeatureSet result;
    // ARMv7-A: the AArch32 features up to Thumb-2 and the barriers
    result.add(ArmFeatures::FEAT_V5T);
    result.add(ArmFeatures::FEAT_V5TE);
    result.add(ArmFeatures::FEAT_V6);
    result.add(ArmFeatures::FEAT_V6M);
    result.add(ArmFeatures::FEAT_V6K);
    result.add(ArmFeatures::FEAT_V6T2);
    result.add(ArmFeatures::FEAT_V7);
    // (LLVM's armv7-a and armv8-a have NEON)
    result.add(ArmFeatures::FEAT_VFP2);
    result.add(ArmFeatures::FEAT_VFP3);
    result.add(ArmFeatures::FEAT_NEON);
    if (major == 7)
        return result;

    // The ARMv9.n versions have the extensions of ARMv8.(n + 5)
    uint level = (major == 9) ? (minor + 5) : minor;

    // ARMv8.0-A
    result.add(ArmFeatures::FEAT_DIV);
    result.add(ArmFeatures::FEAT_DIV_ARM);
    result.add(ArmFeatures::FEAT_V8_AA32);
    result.add(ArmFeatures::FEAT_MP);
    result.add(ArmFeatures::FEAT_VIRT);
    result.add(ArmFeatures::FEAT_TRUSTZONE);
    // (LLVM's armv8-a has the CRC32 instructions of AArch32)
    result.add(ArmFeatures::FEAT_CRC32_AA32);
    result.add(ArmFeatures::FEAT_VFP2);
    result.add(ArmFeatures::FEAT_VFP3);
    result.add(ArmFeatures::FEAT_VFP4);
    result.add(ArmFeatures::FEAT_NEON);
    result.add(ArmFeatures::FEAT_NEON_VFP4);
    // (the floating point of ARMv8, the half precision conversions and the
    // cryptography are in LLVM's armv8-a)
    result.add(ArmFeatures::FEAT_FPARMV8);
    result.add(ArmFeatures::FEAT_FP16_CVT);
    result.add(ArmFeatures::FEAT_AES_AA32);
    result.add(ArmFeatures::FEAT_SHA1_AA32);
    result.add(ArmFeatures::FEAT_SHA256_AA32);
    // ARMv8.1-A
    if (level >= 1)
    {
        result.add(ArmFeatures::FEAT_CRC32);
        result.add(ArmFeatures::FEAT_CRC32_AA32);
        result.add(ArmFeatures::FEAT_LSE);
        result.add(ArmFeatures::FEAT_RDM);
        result.add(ArmFeatures::FEAT_RDM_AA32);
        result.add(ArmFeatures::FEAT_PAN_AA32);
    }
    // ARMv8.2-A
    if (level >= 2)
    {
        result.add(ArmFeatures::FEAT_DPB);
        result.add(ArmFeatures::FEAT_RAS_AA32);
    }
    // ARMv8.3-A
    if (level >= 3)
    {
        result.add(ArmFeatures::FEAT_PAUTH);
        result.add(ArmFeatures::FEAT_FCMA);
        result.add(ArmFeatures::FEAT_FCMA_AA32);
        result.add(ArmFeatures::FEAT_JSCVT);
        result.add(ArmFeatures::FEAT_JSCVT_AA32);
        result.add(ArmFeatures::FEAT_LRCPC);
    }
    // ARMv8.4-A
    if (level >= 4)
    {
        result.add(ArmFeatures::FEAT_LRCPC2);
        result.add(ArmFeatures::FEAT_FLAGM);
        result.add(ArmFeatures::FEAT_DOTPROD);
        result.add(ArmFeatures::FEAT_DOTPROD_AA32);
        result.add(ArmFeatures::FEAT_V8_4A_AA32);
    }
    // ARMv8.5-A (the memory tagging and the random numbers are optional)
    if (level >= 5)
    {
        result.add(ArmFeatures::FEAT_FLAGM2);
        result.add(ArmFeatures::FEAT_FRINTTS);
        result.add(ArmFeatures::FEAT_SB);
        result.add(ArmFeatures::FEAT_SB_AA32);
        result.add(ArmFeatures::FEAT_BTI);
        result.add(ArmFeatures::FEAT_SPECRES);
    }
    // ARMv8.6-A
    if (level >= 6)
    {
        result.add(ArmFeatures::FEAT_BF16);
        result.add(ArmFeatures::FEAT_BF16_AA32);
        result.add(ArmFeatures::FEAT_I8MM);
        result.add(ArmFeatures::FEAT_I8MM_AA32);
    }
    // ARMv8.7-A (the 64 byte loads are optional)
    if (level >= 7)
        result.add(ArmFeatures::FEAT_WFXT);
    // ARMv8.8-A
    if (level >= 8)
    {
        result.add(ArmFeatures::FEAT_MOPS);
        result.add(ArmFeatures::FEAT_HBC);
    }
    // ARMv8.9-A
    if (level >= 9)
        result.add(ArmFeatures::FEAT_CSSC);
    // ARMv9.0-A: SVE and SVE2
    if (major == 9)
    {
        result.add(ArmFeatures::FEAT_SVE);
        result.add(ArmFeatures::FEAT_SVE2);
    }
    // ARMv9.5-A
    if ((major == 9) && (minor >= 5))
    {
        result.add(ArmFeatures::FEAT_CPA);
        result.add(ArmFeatures::FEAT_FAMINMAX);
        result.add(ArmFeatures::FEAT_LUT);
    }
    return result;
}

void ArmFeatureSet::add(ArmFeatures::Feature feature)
{
    CHECK(feature < ArmFeatures::FEATURES_COUNT);
    m_bits[feature / 64] |= (uint64)1 << (feature % 64);
}

void ArmFeatureSet::remove(ArmFeatures::Feature feature)
{
    CHECK(feature < ArmFeatures::FEATURES_COUNT);
    // The base features are on every processor of the instruction set
    if ((feature == ArmFeatures::FEAT_A32_BASE) ||
        (feature == ArmFeatures::FEAT_T32_BASE) ||
        (feature == ArmFeatures::FEAT_A64_BASE))
        return;
    m_bits[feature / 64] &= ~((uint64)1 << (feature % 64));
}

bool ArmFeatureSet::has(ArmFeatures::Feature feature) const
{
    CHECK(feature < ArmFeatures::FEATURES_COUNT);
    return ((m_bits[feature / 64] >> (feature % 64)) & 1) != 0;
}

bool ArmFeatureSet::parse(const char* list)
{
    bool allKnown = true;
    cString text(list);
    text.makeLower();
    cList<cString> items = text.split(XSTL_STRING(","));
    for (cList<cString>::iterator item = items.begin(); item != items.end(); item++)
    {
        cString name(*item);
        name.trim();
        if (name.length() == 0)
            continue;

        // The sign: '+' adds and '-' removes, without one the feature is added
        bool isRemove = false;
        if ((name[0] == XSTL_CHAR('+')) || (name[0] == XSTL_CHAR('-')))
        {
            isRemove = (name[0] == XSTL_CHAR('-'));
            name = name.part(1, name.length());
            name.trim();
        }

        // "none" resets to the base, "all" selects every feature,
        // "armv8.2-a" the features of a version
        if (name == "none")
        {
            *this = ArmFeatureSet();
            continue;
        }
        if (name == "all")
        {
            for (uint i = 0; i < ArmFeatures::FEATURES_COUNT; i++)
            {
                if (isRemove)
                    remove((ArmFeatures::Feature)i);
                else
                    add((ArmFeatures::Feature)i);
            }
            continue;
        }
        if ((name.length() > 4) && (name.left(4) == "armv"))
        {
            XSTL_TRY
            {
                ArmFeatureSet version = fromArchitecture(name.getASCIIstring().getBuffer());
                for (uint i = 0; i < ArmFeatures::FEATURES_COUNT; i++)
                    if (version.has((ArmFeatures::Feature)i))
                    {
                        if (isRemove)
                            remove((ArmFeatures::Feature)i);
                        else
                            add((ArmFeatures::Feature)i);
                    }
            }
            XSTL_CATCH_ALL
            {
                allKnown = false;
            }
            continue;
        }

        // The features of this name (the same LLVM feature can be several)
        bool found = false;
        for (uint i = 0; i < ArmFeatures::FEATURES_COUNT; i++)
        {
            if ((gFeatures[i].m_name[0] == 0) || !(name == gFeatures[i].m_name))
                continue;
            found = true;
            ArmFeatures::Feature feature = (ArmFeatures::Feature)i;
            if (isRemove)
            {
                remove(feature);
                // Removing a feature removes the ones which need it, transitively
                bool changed;
                do
                {
                    changed = false;
                    for (uint j = 0; j < sizeof(gImplications) / sizeof(gImplications[0]); j++)
                        if (!has(gImplications[j].m_implied) && has(gImplications[j].m_feature))
                        {
                            remove(gImplications[j].m_feature);
                            changed = true;
                        }
                } while (changed);
            } else
            {
                add(feature);
                // Add the features which it implies, transitively
                bool changed;
                do
                {
                    changed = false;
                    for (uint j = 0; j < sizeof(gImplications) / sizeof(gImplications[0]); j++)
                        if (has(gImplications[j].m_feature) &&
                            !has(gImplications[j].m_implied))
                        {
                            add(gImplications[j].m_implied);
                            changed = true;
                        }
                } while (changed);
            }
        }
        if (!found)
            allKnown = false;
    }
    return allKnown;
}

bool ArmFeatureSet::operator == (const ArmFeatureSet& other) const
{
    for (uint i = 0; i < WORDS_COUNT; i++)
        if (m_bits[i] != other.m_bits[i])
            return false;
    return true;
}
