#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVFEATURES_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVFEATURES_H

/*
 * RiscvFeatures.h
 *
 * The extensions of the RISC-V processors, named after the ratified
 * extension names (the names of the LLVM target features), their place in
 * the RVA23 profiles, and what dismount supports of each one.
 *
 * Every encoding of the instruction tables is marked with the feature which
 * adds it (RiscvEncodingEntry::m_feature, and m_feature2 for the encodings
 * which need two: c.fld is Zcd, fli.h is Zfa with Zfh, amocas.h is Zabha
 * with Zacas). The disassembler and the assembler take the set of the
 * features of the target processor: an encoding of a feature which isn't in
 * the set is not decoded (the bytes are an invalid instruction) and is not
 * assembled.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/riscv/RiscvInstructionSet.h"

class RiscvFeatures {
public:
    /*
     * The features. FEAT_I is the base of both RV32 and RV64: an encoding
     * marked FEAT_I exists on every processor (RV32I / RV64I).
     *
     * Some features have no instructions of their own: a property of the
     * memory system (Ziccif, Za64rs...), of the privileged architecture
     * (Svade, Ssu64xl...), or control and status registers only (Sstc's
     * stimecmp, Zkr's seed). They are features of the set all the same, so a
     * profile is a set of features, and the names of their CSRs are decoded
     * and assembled only when they are in the set (the CSR names of LLVM).
     * getSupport() marks them SUPPORT_NO_INSTRUCTIONS.
     *
     * The shorthand names of the specifications ("g", "a", "b", "c", "zk",
     * "zkn", "zks", "zvkn", "zvkng", "zvks", "zvksg", "sha"...) are not
     * features: RiscvFeatureSet::parse() expands them.
     *
     * The values are bit numbers of RiscvFeatureSet; add new features before
     * FEATURES_COUNT and give them a name and an RVA23 status in
     * RiscvFeatures.cpp.
     */
    enum Feature {
        // --- The base and the classic single letter extensions ------------
        // RV32I / RV64I: the integer, branch, load / store, fence, ecall,
        // ebreak
        FEAT_I,
        // The integer multiply (mul, mulh, mulhsu, mulhu, mulw) and divide
        // (div, divu, rem, remu and their w forms). M is Zmmul with the
        // divides: the multiplies are marked FEAT_ZMMUL, the divides FEAT_M
        FEAT_M,
        FEAT_ZMMUL,
        // The atomics: A is Zalrsc (lr, sc) and Zaamo (amoswap, amoadd...)
        FEAT_ZALRSC,
        FEAT_ZAAMO,
        // Single and double precision floating point
        FEAT_F,
        FEAT_D,

        // --- The integer extensions of the unprivileged ISA ---------------
        // The CSR instructions (csrrw...) and the counters (rdcycle, rdtime,
        // rdinstret: Zicntr; hpmcounter3-31: Zihpm, their CSR names)
        FEAT_ZICSR,
        FEAT_ZICNTR,
        FEAT_ZIHPM,
        // fence.i
        FEAT_ZIFENCEI,
        // pause (fence w, 0), the non temporal hints (ntl.p1, ntl.pall...,
        // c.ntl.*: add / c.add to x0)
        FEAT_ZIHINTPAUSE,
        FEAT_ZIHINTNTL,
        // The cache block management (cbo.clean, cbo.flush, cbo.inval), the
        // prefetches (prefetch.i / r / w, ori hints) and cbo.zero
        FEAT_ZICBOM,
        FEAT_ZICBOP,
        FEAT_ZICBOZ,
        // czero.eqz, czero.nez
        FEAT_ZICOND,
        // The may be operations: mop.r.0-31, mop.rr.0-7 (they write 0 to rd
        // unless a later extension redefines them, Zicfiss does)
        FEAT_ZIMOP,
        // wrs.nto, wrs.sto
        FEAT_ZAWRS,
        // amocas.w / d / q (amocas.d on RV32 and amocas.q on RV64 take even
        // register pairs)
        FEAT_ZACAS,
        // The byte and halfword atomics (amoadd.b..., amocas.b / h with Zacas)
        FEAT_ZABHA,
        // Control flow integrity: lpad (landing pads), and the shadow stack
        // (sspush, sspopchk, ssrdp, ssamoswap.w / d, c.sspush, c.sspopchk, on
        // the Zimop / Zcmop encodings)
        FEAT_ZICFILP,
        FEAT_ZICFISS,

        // --- Bit manipulation and scalar cryptography ---------------------
        // B is Zba + Zbb + Zbs
        FEAT_ZBA,           // sh1add, sh2add, sh3add, add.uw, slli.uw...
        FEAT_ZBB,           // andn, orn, xnor, clz, ctz, cpop, min, max,
                            // sext.b / h, zext.h, rol, ror, orc.b, rev8
        FEAT_ZBS,           // bclr, bext, binv, bset
        FEAT_ZBC,           // clmul, clmulh, clmulr
        FEAT_ZBKB,          // pack, packh, packw, brev8, zip, unzip
        FEAT_ZBKC,          // clmul, clmulh
        FEAT_ZBKX,          // xperm4, xperm8
        FEAT_ZKND,          // aes32dsi / aes64ds..., aes64im, aes64ks1i / 2
        FEAT_ZKNE,          // aes32esi / aes64es...
        FEAT_ZKNH,          // sha256sig0..., sha512sig0...
        FEAT_ZKSED,         // sm4ed, sm4ks
        FEAT_ZKSH,          // sm3p0, sm3p1
        FEAT_ZKR,           // the seed CSR (no instructions)
        FEAT_ZKT,           // data independent execution latency (none)

        // --- Floating point extensions -----------------------------------
        // fli, fminm / fmaxm, fround / froundnx, fcvtmod.w.d, fmvh.x.d /
        // fmvp.d.x (RV32), fleq / fltq; the .h forms need Zfh, the .d forms D
        FEAT_ZFA,
        // Half precision: the full arithmetic (Zfh), or the loads, stores,
        // moves and conversions only (Zfhmin)
        FEAT_ZFH,
        FEAT_ZFHMIN,
        // BFloat16 conversions: fcvt.bf16.s, fcvt.s.bf16
        FEAT_ZFBFMIN,

        // --- Compressed instructions -------------------------------------
        // C is Zca, with Zcf (RV32 with F: c.flw...) and Zcd (with D: c.fld...)
        FEAT_ZCA,
        FEAT_ZCF,
        FEAT_ZCD,
        // c.lbu, c.lhu, c.lh, c.sb, c.sh, c.zext.b, c.sext.b / h (Zbb),
        // c.zext.h (Zbb), c.zext.w (Zba), c.not, c.mul (Zmmul)
        FEAT_ZCB,
        // c.mop.1-15
        FEAT_ZCMOP,

        // --- The vector extension and its subsets ------------------------
        // The embedded subsets: integer elements of up to 32 / 64 bits
        // (Zve32x, Zve64x), with single (Zve32f, Zve64f) and double (Zve64d)
        // precision. V is Zve64d with VLEN >= 128. An encoding is marked with
        // the subset LLVM requires for it (the integer instructions Zve32x,
        // the float ones Zve32f, vle64.v / vluxei64.v Zve64x); the element
        // width of an instruction comes from vtype, so the 64 bit elements of
        // the Zve32 subsets are checked by the processor at runtime
        FEAT_ZVE32X,
        FEAT_ZVE32F,
        FEAT_ZVE64X,
        FEAT_ZVE64F,
        FEAT_ZVE64D,
        FEAT_V,
        // Half precision vectors: the arithmetic (Zvfh), the conversions only
        // (Zvfhmin)
        FEAT_ZVFH,
        FEAT_ZVFHMIN,
        // BFloat16 vectors: vfncvtbf16.f.f.w / vfwcvtbf16.f.f.v (Zvfbfmin),
        // vfwmaccbf16 (Zvfbfwma)
        FEAT_ZVFBFMIN,
        FEAT_ZVFBFWMA,
        // Vector bit manipulation: vandn, vbrev8, vrev8, vrol, vror (Zvkb);
        // vbrev, vclz, vctz, vcpop.v, vwsll (Zvbb, which includes Zvkb)
        FEAT_ZVKB,
        FEAT_ZVBB,
        // vclmul, vclmulh
        FEAT_ZVBC,
        // Vector cryptography: GHASH (vghsh, vgmul), AES (vaesef...,
        // vaeskf1 / 2, vaesz), SHA-2 (vsha2ms, vsha2ch / cl: Zvknha for SHA-256,
        // Zvknhb for SHA-256 and SHA-512, the same encodings), SM4 (vsm4k,
        // vsm4r), SM3 (vsm3me, vsm3c), the data independent latency (Zvkt,
        // none)
        FEAT_ZVKG,
        FEAT_ZVKNED,
        FEAT_ZVKNHA,
        FEAT_ZVKNHB,
        FEAT_ZVKSED,
        FEAT_ZVKSH,
        FEAT_ZVKT,

        // --- Properties without instructions of the RVA23 profiles -------
        FEAT_ZICCIF,        // instruction fetch atomicity
        FEAT_ZICCRSE,       // reservation set forward progress
        FEAT_ZICCAMOA,      // all AMOs on cacheable memory
        FEAT_ZICCAMOC,      // AMOs as compare and swap on cacheable memory
        FEAT_ZICCLSM,       // misaligned loads and stores
        FEAT_ZA64RS,        // 64 bytes reservation sets
        FEAT_ZIC64B,        // 64 bytes cache blocks
        FEAT_ZAMA16B,       // misaligned atomicity in 16 bytes
        FEAT_SUPM,          // pointer masking in the user mode

        // --- The privileged architecture --------------------------------
        // The base privileged instructions (mret, sret, wfi, sfence.vma) and
        // the CSR names of the machine and supervisor modes. LLVM has no
        // feature for them: they are always decoded, the name is ""
        FEAT_PRIVILEGED,
        // sinval.vma, sfence.w.inval, sfence.inval.ir (hinval.vvma / gvma
        // with H)
        FEAT_SVINVAL,
        // The hypervisor: hfence.vvma / gvma, hlv.*, hlvx.*, hsv.*, the h / vs
        // CSRs
        FEAT_H,
        // The supervisor features of RVA23S64 (the CSRs and the behaviors),
        // without instructions
        FEAT_SSTC,
        FEAT_SSCOFPMF,
        FEAT_SSNPM,
        FEAT_SSSTATEEN,
        FEAT_SVPBMT,
        FEAT_SVNAPOT,
        FEAT_SVADE,
        FEAT_SVBARE,
        FEAT_SSCCPTR,
        FEAT_SSTVECD,
        FEAT_SSTVALA,
        FEAT_SSCOUNTERENW,
        FEAT_SSU64XL,
        FEAT_SHCOUNTERENW,
        FEAT_SHVSTVALA,
        FEAT_SHTVALA,
        FEAT_SHVSTVECD,
        FEAT_SHVSATPA,
        FEAT_SHGATPA,
        // The optional supervisor features of RVA23S64
        FEAT_SDTRIG,
        FEAT_SSSTRICT,
        FEAT_SVVPTC,
        FEAT_SSPM,

        // The number of features. Must be last
        FEATURES_COUNT
    };

    /*
     * What dismount does with the instructions of a feature, for each
     * component. A feature is marked with the components which handle every
     * instruction of the feature; a component which handles only some of
     * them marks the feature SUPPORT_PARTIAL_LIFT instead (the same bits as
     * ArmFeatures::Support).
     */
    enum Support {
        // The instructions are decoded and formatted (the LLVM syntax)
        SUPPORT_DISASSEMBLE          = 0x01,
        // The instructions are assembled from the LLVM syntax
        SUPPORT_ASSEMBLE             = 0x02,
        // The instructions are lifted to the semantic IR
        SUPPORT_LIFT                 = 0x04,
        // Some of the instructions are lifted (the others are OP_UNDEFINED),
        // see the comment of the feature in RiscvFeatures.cpp
        SUPPORT_PARTIAL_LIFT         = 0x08,
        // The lifted instructions are compared with a reference processor
        // (QEMU, Unicorn), see tests/riscv/TEST_PLAN.md
        SUPPORT_LIFT_VERIFIED        = 0x10,
        // The decoder and the assembler are compared with LLVM (llvm-mc and
        // llvm-objdump), see tests/riscv/TEST_PLAN.md
        SUPPORT_ENCODING_VERIFIED    = 0x20,
        // The feature has no instructions of its own (see Feature): there is
        // nothing to decode or lift. Its CSR names, if any, are covered by
        // SUPPORT_DISASSEMBLE / SUPPORT_ASSEMBLE
        SUPPORT_NO_INSTRUCTIONS      = 0x40
    };

    /*
     * The place of a feature in the RVA23 profiles (RVA23U64 for the user
     * mode, RVA23S64 for the supervisor mode, which includes RVA23U64), as
     * the ratified profile specification lists it
     */
    enum ProfileStatus {
        // Not in the profiles (scalar cryptography, Zbc of RV32 targets...)
        PROFILE_NONE,
        // Mandatory in RVA23U64 (and so in RVA23S64)
        PROFILE_RVA23U64_MANDATORY,
        // Optional in RVA23U64 (Zvkng, Zvksg, Zacas, Zabha, Zfh, Zvfh, Zbc,
        // Zicfilp, Zicfiss, Zfbfmin, Zvfbfmin, Zvfbfwma, Zama16b, Ziccamoc,
        // Zvbc...)
        PROFILE_RVA23U64_OPTIONAL,
        // Mandatory in RVA23S64 only (Zifencei, H, Svinval, Sstc...)
        PROFILE_RVA23S64_MANDATORY,
        // Optional in RVA23S64 only (Sdtrig, Ssstrict, Svvptc, Sspm...)
        PROFILE_RVA23S64_OPTIONAL
    };

    /*
     * Return the name of a feature: the name of the LLVM target feature
     * ("m", "zba", "zvkned", "experimental-zicfilp"...) which tests/riscv
     * passes to llvm-mc / llvm-objdump --mattr. FEAT_I and FEAT_PRIVILEGED
     * return "" (always on)
     */
    static const char* getName(Feature feature);

    /*
     * Find a feature by its LLVM name. The name of the specification is
     * accepted too, without the "experimental-" prefix ("zicfilp")
     *
     * Return false if the name is unknown
     */
    static bool findFeature(const char* name, Feature& feature);

    /*
     * Return the place of a feature in the RVA23 profiles
     */
    static ProfileStatus getProfileStatus(Feature feature);

    /*
     * Return true if the feature exists in an instruction set (Zcf and the
     * RV32 only parts of Zbkb exist in RV32 only; the others in both)
     */
    static bool isAvailable(Feature feature, RiscvInstructionSet::DisassemblerTypes type);

    /*
     * Return the Support bits of a feature: what dismount does with its
     * instructions. This is the support matrix of tests/riscv/SUPPORT.md,
     * which is generated from it (tests/riscv/tools/gen_support_matrix.py)
     */
    static uint getSupport(Feature feature);
};

/*
 * A set of RiscvFeatures::Feature, the features of a target processor.
 *
 * NOTE: This class is a value, it can be copied freely.
 */
class RiscvFeatureSet {
public:
    /*
     * Constructor: the base only (FEAT_I and FEAT_PRIVILEGED)
     */
    RiscvFeatureSet();

    /*
     * Return the set of all the features (the default of the disassembler:
     * decode everything). The features which conflict are resolved for the
     * widest decoding: Zcmop and the Zicfiss compressed forms are on (the
     * c.mop encodings print as c.sspush / c.sspopchk where Zicfiss defines
     * them, as LLVM does with both)
     */
    static RiscvFeatureSet all();

    /*
     * Return the features of a profile, with its mandatory extensions:
     * "rva23u64", "rva23s64", "rva22u64", "rva22s64", "rva20u64",
     * "rvi20u32", "rvi20u64" (the profiles of LLVM). The optional
     * extensions are not included; add them with parse().
     *
     * Throw exception for an unknown profile
     */
    static RiscvFeatureSet fromProfile(const char* profile);

    // Add / remove / test a feature
    void add(RiscvFeatures::Feature feature);
    void remove(RiscvFeatures::Feature feature);
    bool has(RiscvFeatures::Feature feature) const;

    /*
     * Add or remove the features of a list. Two syntaxes are accepted:
     *   - The LLVM feature list: "+v,+zvbb,-c" (without a sign the feature is
     *     added to the set)
     *   - An ISA string (-march): "rv64gcv_zba_zbb_zicond" or a profile with
     *     extensions "rva23u64_zvkng". It describes the whole target: the set
     *     is replaced by the base and the features of the string. The XLEN of
     *     an ISA string is not a feature, the instruction set is given
     *     separately.
     * A feature brings the features it needs, as LLVM implies them ("+v"
     * adds zve64d, zve32f, f, d, zicsr; "+zvkng" adds zvkned, zvknhb, zvkg,
     * zvkb, zvkt; "+zcd" adds zca and d), and the shorthands expand ("g" is
     * i, m, a, f, d, zicsr, zifencei; "c" is zca with zcf / zcd when f / d
     * are in the set; "b" is zba, zbb, zbs). Two words are not features:
     * "none" resets the set to the base and "all" is every feature.
     *
     * LLVM's rules: removing a feature (-zca) removes the ones which need it
     * (zcb, zcd, zcf, zcmop); "c" and "zce" are Zca (Zce: Zcb), with Zcd when
     * d is in the set and Zcf when f is (the zcf bit of an RV64 set is inert:
     * its encodings are RV32 only), whatever the order of the list is. "-c"
     * removes Zca (LLVM only turns the "c" feature off). The names "relax" and
     * "zvl<n>b" (the minimum vector length) are accepted and ignored.
     *
     * Return false if a name is unknown (the known names are still applied)
     */
    bool parse(const char* list);

    // Compare two sets
    bool operator == (const RiscvFeatureSet& other) const;

private:
    enum { WORDS_COUNT = (RiscvFeatures::FEATURES_COUNT + 63) / 64 };
    // The bits, feature n is bit n % 64 of word n / 64
    uint64 m_bits[WORDS_COUNT];
};

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVFEATURES_H
