#ifndef __TBA_DISMOUNT_PROC_ARM_ARMFEATURES_H
#define __TBA_DISMOUNT_PROC_ARM_ARMFEATURES_H

/*
 * ArmFeatures.h
 *
 * The architecture versions and the extensions of the ARM processors, named
 * after the FEAT_* names of the Arm Architecture Reference Manual, and what
 * dismount supports of each one.
 *
 * Every encoding of the instruction tables is marked with the one feature
 * which adds it (ArmEncodingEntry::m_feature). The disassembler and the
 * assembler take the set of the features of the target processor: an
 * encoding of a feature which isn't in the set is not decoded (the bytes are
 * an invalid instruction) and is not assembled.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/arm/ArmInstructionSet.h"

class ArmFeatures {
public:
    /*
     * The features. The base of each instruction set is the first entry of
     * its group: an encoding marked FEAT_A32_BASE, FEAT_T32_BASE or
     * FEAT_A64_BASE exists on every processor of the instruction set.
     *
     * The values are bit numbers of ArmFeatureSet; add new features before
     * FEATURES_COUNT and give them a name in ArmFeatures.cpp.
     */
    enum Feature {
        // --- AArch32 (A32 and T32) -------------------------------------
        // ARMv4T: the A32 base, and the 16 bit Thumb of ARMv4T
        FEAT_A32_BASE,
        FEAT_T32_BASE,
        // ARMv5T / ARMv5TE: blx, clz, bkpt, ldrd / strd, pld, qadd / qsub,
        // smla / smlal / smul / smulw
        FEAT_V5TE,
        // ARMv6: rev, sxtb / uxtb, the parallel add and subtract (sadd16,
        // uqsub8...), sel, ssat / usat, ldrex / strex, cps, srs / rfe, setend,
        // smlad / smuad, umaal, the 16 bit Thumb cpy / rev / sxt
        FEAT_V6,
        // ARMv6K: ldrexb / h / d, clrex, yield / wfe / wfi / sev, the 16 bit
        // Thumb hints
        FEAT_V6K,
        // ARMv6T2: Thumb-2 (the 32 bit Thumb encodings), movw / movt, bfi /
        // bfc / sbfx / ubfx, rbit, mls, ldrt / strt in Thumb, it
        FEAT_V6T2,
        // ARMv7: dmb / dsb / isb, pli, dbg, pldw (FEAT_MP)
        FEAT_V7,
        // sdiv / udiv (ARMv7-R, ARMv7VE, all ARMv8)
        FEAT_DIV,
        // ARMv8 AArch32: lda / stl / ldaex / stlex, sevl, hlt, the vsel / vmaxnm
        // / vrint / vcvta FP and the v8 NEON (vmaxnm, vrint, vcvta)
        FEAT_V8_AA32,
        // The AArch32 crc32 instructions (crc32b... crc32cw)
        FEAT_CRC32_AA32,
        // The AArch32 Cryptographic Extension: aese / aesd / aesmc / aesimc,
        // sha1 / sha256, vmull.p64
        FEAT_AES_AA32,
        FEAT_SHA1_AA32,
        FEAT_SHA256_AA32,
        // VFPv2: the floating point of ARMv5 / v6 (vadd.f32 / f64, vldr,
        // vmov, vmrs...), 16 double registers
        FEAT_VFP2,
        // VFPv3: vmov immediate, the fixed point conversions, d16-d31
        // (VFPv3-D32)
        FEAT_VFP3,
        // VFPv4: vfma / vfms / vfnma / vfnms, the half precision conversions
        // vcvtb / vcvtt (FEAT_FP16 conversions of VFPv3-FP16 included)
        FEAT_VFP4,
        // Advanced SIMD of ARMv7 (NEON): the q registers, vadd.i8..., vld1-4
        FEAT_NEON,
        // NEON of VFPv4: vfma.f32 / vfms.f32 on vectors
        FEAT_NEON_VFP4,
        // ARMv8.1 AArch32: vqrdmlah / vqrdmlsh
        FEAT_RDM_AA32,
        // ARMv8.2 AArch32 half precision arithmetic (vadd.f16...)
        FEAT_FP16_AA32,
        // ARMv8.2 AArch32 vsdot / vudot, ARMv8.6 vsmmla / vusdot / vsudot,
        // vmmla.bf16 / vdot.bf16 / vfmab / vcvt.bf16
        FEAT_DOTPROD_AA32,
        FEAT_I8MM_AA32,
        FEAT_BF16_AA32,
        // ARMv8.3 AArch32 vcadd / vcmla, vjcvt
        FEAT_FCMA_AA32,
        FEAT_JSCVT_AA32,
        // ARMv8.2 AArch32 vfmal / vfmsl
        FEAT_FHM_AA32,
        // ARMv8-M / ARMv8.1-M Thumb only extensions are not supported (MVE,
        // the security extension sg / tt, the low overhead loops)

        // --- AArch64 (A64) ----------------------------------------------
        // ARMv8.0-A: the integer, branch, load / store and system
        // instructions, the scalar floating point (FEAT_FP) and Advanced
        // SIMD (FEAT_AdvSIMD, NEON), which every A64 processor has
        FEAT_A64_BASE,
        // The A64 Cryptographic Extension and its parts
        FEAT_AES,           // aese, aesd, aesmc, aesimc, pmull{2}.1q
        FEAT_SHA1,          // sha1c, sha1h, sha1m, sha1p, sha1su0 / 1
        FEAT_SHA256,        // sha256h / h2, sha256su0 / 1
        FEAT_SHA512,        // ARMv8.2 sha512h / h2, sha512su0 / 1
        FEAT_SHA3,          // ARMv8.2 eor3, bcax, rax1, xar
        FEAT_SM3,           // ARMv8.2 sm3ss1, sm3tt*, sm3partw1 / 2
        FEAT_SM4,           // ARMv8.2 sm4e, sm4ekey
        // crc32b... crc32cx (ARMv8.0 optional, ARMv8.1 mandatory)
        FEAT_CRC32,
        // ARMv8.1: the large system extensions (cas, casp, swp, ldadd, ldclr,
        // ldeor, ldset, ldsmax..., stadd...)
        FEAT_LSE,
        // ARMv8.1: sqrdmlah / sqrdmlsh
        FEAT_RDM,
        // ARMv8.2 half precision arithmetic (fadd h0..., fadd v0.8h...)
        FEAT_FP16,
        // ARMv8.2 fmlal / fmlsl
        FEAT_FHM,
        // ARMv8.2 sdot / udot
        FEAT_DOTPROD,
        // ARMv8.2 dc cvap / ARMv8.5 dc cvadp
        FEAT_DPB,
        // ARMv8.3 pointer authentication (pacia, autia, retaa, braa, ldraa...)
        FEAT_PAUTH,
        // ARMv8.3 fcadd / fcmla
        FEAT_FCMA,
        // ARMv8.3 fjcvtzs
        FEAT_JSCVT,
        // ARMv8.3 ldapr, ARMv8.4 ldapur / stlur (FEAT_LRCPC2)
        FEAT_LRCPC,
        FEAT_LRCPC2,
        // ARMv8.4 cfinv, rmif, setf8 / setf16; ARMv8.5 axflag / xaflag
        FEAT_FLAGM,
        FEAT_FLAGM2,
        // ARMv8.5 frint32x / frint32z / frint64x / frint64z
        FEAT_FRINTTS,
        // ARMv8.5 sb; branch target identification (bti)
        FEAT_SB,
        FEAT_BTI,
        // ARMv8.5 the memory tagging extension (irg, gmi, addg, subg, stg,
        // ldg, st2g, stzg...)
        FEAT_MTE,
        // ARMv8.5 rndr / rndrrs (system registers)
        FEAT_RNG,
        // ARMv8.0 optional prediction restriction (cfp / dvp / cpp rctx)
        FEAT_SPECRES,
        // ARMv8.6 smmla / ummla / usmmla / usdot / sudot
        FEAT_I8MM,
        // ARMv8.6 bfdot, bfmmla, bfmlalb / t, bfcvt / bfcvtn
        FEAT_BF16,
        // ARMv8.7 ld64b / st64b / st64bv / st64bv0; wfet / wfit (FEAT_WFxT)
        FEAT_LS64,
        FEAT_WFXT,
        // ARMv8.8 the memory copy and set instructions (cpyfp..., setp...);
        // bc.cond (FEAT_HBC)
        FEAT_MOPS,
        FEAT_HBC,
        // ARMv8.9 / ARMv9.4: abs, cnt, ctz, smax..., umin... on general
        // registers (FEAT_CSSC); ldiapp / stilp / ldapr post-index (LRCPC3);
        // ldclrp / ldsetp / swpp (LSE128); the 128 bit system registers
        // (mrrs / msrr, FEAT_SYSREG128)
        FEAT_CSSC,
        FEAT_LRCPC3,
        FEAT_LSE128,
        FEAT_SYSREG128,
        // ARMv8.9 Guarded control stack (gcspushm, gcsss1...); the checked
        // pointer arithmetic (addpt, subpt, maddpt: FEAT_CPA)
        FEAT_GCS,
        FEAT_CPA,
        // ARMv9.5 / 2024: the FP8 conversions and arithmetic of Advanced SIMD
        // (fcvtn.8b, fmlalb.8h..., FEAT_FP8, FEAT_FP8FMA, FEAT_FP8DOT2 / 4),
        // faminmax (FEAT_FAMINMAX), luti2 / luti4 on v registers (FEAT_LUT)
        FEAT_FP8,
        FEAT_FP8FMA,
        FEAT_FP8DOT4,
        FEAT_FP8DOT2,
        FEAT_FAMINMAX,
        FEAT_LUT,

        // --- The scalable vector extensions of A64 -----------------------
        // SVE (ARMv8.2): z0-z31, p0-p15, ffr, the predicated integer and
        // floating point, gather / scatter and first faulting loads
        FEAT_SVE,
        // SVE matrix multiply: fmmla .s (FEAT_F32MM), fmmla .d and ld1rob
        // (FEAT_F64MM). smmla / ummla / usmmla / usdot / sudot on z are
        // FEAT_I8MM with FEAT_SVE, bfdot / bfmmla on z FEAT_BF16 with FEAT_SVE
        FEAT_F32MM,
        FEAT_F64MM,
        // SVE2 (ARMv9.0): the widening, narrowing, saturating, complex,
        // histogram, match and the bit permute / crypto parts
        FEAT_SVE2,
        FEAT_SVE_AES,       // aese / aesd / aesmc / aesimc / pmullb.q on z
        FEAT_SVE_SHA3,      // rax1 on z
        FEAT_SVE_SM4,       // sm4e / sm4ekey on z
        FEAT_SVE_BITPERM,   // bdep, bext, bgrp
        // SVE2.1 (ARMv9.4): ld1q / st1q, the quadword reductions (addqv...),
        // pext / ptrue counter, the multi vector loads (ld1b {z0.b-z1.b})
        FEAT_SVE2P1,
        // SVE BFloat16 arithmetic (bfadd, bfmla... on z, FEAT_SVE_B16B16)
        FEAT_SVE_B16B16,

        // --- The scalable matrix extensions of A64 -----------------------
        // SME (ARMv9.2): za, the streaming mode (smstart / smstop), the outer
        // products fmopa / smopa / bfmopa, mova / ld1b / st1b of ZA slices,
        // addha / addva, zero {za}
        FEAT_SME,
        // SME optional parts: fmopa .d (FEAT_SME_F64F64), smopa .d / addha .d
        // (FEAT_SME_I16I64), fmopa .h (FEAT_SME_F16F16)
        FEAT_SME_F64F64,
        FEAT_SME_I16I64,
        FEAT_SME_F16F16,
        // SME2 (ARMv9.3): zt0, the multi vector instructions (fmla za.s[w8, 0,
        // vgx2]...), luti2 / luti4 on zt0, the predicate as counter (pn
        // registers, whilelo pn8.b), the multi vector loads
        FEAT_SME2,
        // SME2.1 (ARMv9.4): movaz, zero za.d[w8, 0, vgx2], luti4 multi vector
        FEAT_SME2P1,
        // SME2 FP8 and the lookup tables of SME2 (fmopa za.h with fp8,
        // luti4 4 registers: FEAT_SME_LUTv2)
        FEAT_SME_LUTV2,

        // --- Extensions of the A64 system and base instructions which the
        // LLVM disassembler knows (added with the base table)
        // ARMv8.7 dsb <option>nXS (FEAT_XS)
        FEAT_XS,
        // Transactional memory: tstart, tcommit, ttest, tcancel (FEAT_TME)
        FEAT_TME,
        // Unprivileged load / store instructions (ldtp, sttp, ldtxr...)
        FEAT_LSUI,
        // Compare and branch (cbgt, cbbhs...)
        FEAT_CMPBR,
        // Limited ordering regions (ldlar, stllr)
        FEAT_LOR,
        // Translation hardening extension: rcwclr, rcwset, rcwswp, rcwcas...
        FEAT_THE,
        // Atomic floating point instructions (ldfadd, ldbfadd, stfmax...)
        FEAT_LSFE,
        // Pointer authentication with the return address in the pc
        // (retaasppc, autiasppc...)
        FEAT_PAUTH_LR,
        // Statistical profiling (psb csync), trace filtering (tsb csync), RAS
        // (esb), the store hint (stshh), permission overlays 2 (tchangef),
        // the transactional exit (texit)
        FEAT_SPE,
        FEAT_TRF,
        FEAT_RAS,
        FEAT_PCDPHINT,
        FEAT_POE2,
        FEAT_TEV,
        // A64 SIMD: the FP8 matrix multiply of Advanced SIMD (fmmla
        // v0.4s, v1.16b, v2.16b: FEAT_F8F32MM, v0.8h: FEAT_F8F16MM) and the
        // conversions between floating point registers without the general
        // registers (fcvtas s0, d1: FEAT_FPRCVT)
        FEAT_F8F32MM,
        FEAT_F8F16MM,
        FEAT_FPRCVT,

        // The number of features. Must be last
        FEATURES_COUNT
    };

    /*
     * What dismount does with the instructions of a feature, for each
     * component. A feature is marked with the components which handle every
     * instruction of the feature; a component which handles only some of
     * them marks the feature SUPPORT_PARTIAL_* instead.
     */
    enum Support {
        // The instructions are decoded and formatted (the LLVM syntax)
        SUPPORT_DISASSEMBLE          = 0x01,
        // The instructions are assembled from the LLVM syntax
        SUPPORT_ASSEMBLE             = 0x02,
        // The instructions are lifted to the semantic IR
        SUPPORT_LIFT                 = 0x04,
        // Some of the instructions are lifted (the others are OP_UNDEFINED),
        // see the comment of the feature in ArmFeatures.cpp
        SUPPORT_PARTIAL_LIFT         = 0x08,
        // The lifted instructions are compared with a reference processor
        // (Unicorn, or the host for A64), see tests/arm/TEST_PLAN.md
        SUPPORT_LIFT_VERIFIED        = 0x10,
        // The decoder and the assembler are compared with LLVM (clang and
        // objdump), see tests/arm/TEST_PLAN.md
        SUPPORT_ENCODING_VERIFIED    = 0x20
    };

    /*
     * Return the name of a feature: the name of the LLVM target feature
     * ("sve2", "lse", "neon", "crc"...), which tests/arm passes to clang
     * -march / objdump --mattr. The base features return "" (always on)
     */
    static const char* getName(Feature feature);

    /*
     * Find a feature by its LLVM name
     *
     * Return false if the name is unknown
     */
    static bool findFeature(const char* name, Feature& feature);

    /*
     * Return the instruction set of a feature: A32 for the AArch32 features
     * which exist in both A32 and T32 (FEAT_V5TE... FEAT_FHM_AA32), T32 for
     * FEAT_T32_BASE and FEAT_V6T2, A64 for the others
     */
    static ArmInstructionSet::DisassemblerTypes getInstructionSet(Feature feature);

    /*
     * Return the Support bits of a feature: what dismount does with its
     * instructions. This is the support matrix of tests/arm/SUPPORT.md, which
     * is generated from it (tests/arm/tools/gen_support_matrix.py)
     */
    static uint getSupport(Feature feature);
};

/*
 * A set of ArmFeatures::Feature, the features of a target processor.
 *
 * NOTE: This class is a value, it can be copied freely.
 */
class ArmFeatureSet {
public:
    /*
     * Constructor: the base features of all the instruction sets only
     * (FEAT_A32_BASE, FEAT_T32_BASE, FEAT_A64_BASE)
     */
    ArmFeatureSet();

    /*
     * Return the set of all the features (the default of the disassembler:
     * decode everything, like objdump --mattr=+all)
     */
    static ArmFeatureSet all();

    /*
     * Return the features of an architecture version, with the extensions
     * which are mandatory in it ("armv7-a", "armv8-a", "armv8.2-a",
     * "armv9-a", "armv9.4-a"...). The optional extensions are not included.
     *
     * Throw exception for an unknown version
     */
    static ArmFeatureSet fromArchitecture(const char* version);

    // Add / remove / test a feature
    void add(ArmFeatures::Feature feature);
    void remove(ArmFeatures::Feature feature);
    bool has(ArmFeatures::Feature feature) const;

    /*
     * Add or remove the features of an LLVM feature list: "+sve2,+sme,-neon"
     * (without a sign the feature is added). A feature brings the features
     * it needs ("+sve2" adds sve). Three words are not features: "none"
     * resets the set to the base features, "all" is every feature and
     * "armv8.2-a" the features of a version (fromArchitecture()).
     *
     * Return false if a name is unknown (the known names are still applied)
     */
    bool parse(const char* list);

    // Compare two sets
    bool operator == (const ArmFeatureSet& other) const;

private:
    enum { WORDS_COUNT = (ArmFeatures::FEATURES_COUNT + 63) / 64 };
    // The bits, feature n is bit n % 64 of word n / 64
    uint64 m_bits[WORDS_COUNT];
};

#endif // __TBA_DISMOUNT_PROC_ARM_ARMFEATURES_H
