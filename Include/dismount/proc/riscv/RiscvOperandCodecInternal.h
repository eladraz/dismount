#ifndef __TBA_DISMOUNT_PROC_RISCV_RISCVOPERANDCODECINTERNAL_H
#define __TBA_DISMOUNT_PROC_RISCV_RISCVOPERANDCODECINTERNAL_H

/*
 * RiscvOperandCodecInternal.h
 *
 * The helpers which the files of RiscvOperandCodec*.cpp share: the immediate
 * schemes, the vector kinds. Not part of the interface.
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/proc/riscv/RiscvOperandCodec.h"

namespace riscvdis {

/*
 * Decode / encode an immediate by its scheme (RiscvImmediateScheme).
 *
 * decodeImmediate returns the value of the scheme from the bits (signed or
 * unsigned as the scheme is), false if the bits are reserved for the scheme.
 * encodeImmediate sets the bits of the scheme from the value (the bits of the
 * scheme are cleared first), false if the value doesn't fit (range, alignment).
 */
bool decodeImmediate(uint scheme, uint32 encoding, int64& value);
bool encodeImmediate(uint scheme, int64 value, uint32& encoding);

// Return true if the scheme gives an unsigned value
bool isUnsignedScheme(uint scheme);

// Sign extend the low 'bits' bits of a value
int64 signExtend(uint64 value, uint bits);

/*
 * The vector kinds (RiscvOperandCodecVector.cpp): RISCV_KIND_VR,
 * RISCV_KIND_VR_V0, RISCV_KIND_VMASK, RISCV_KIND_VTYPE
 */
bool decodeVectorKind(const RiscvOperandSpec& spec, uint32 encoding,
                      RiscvDecodedOperand& operand);
bool encodeVectorKind(const RiscvOperandSpec& spec,
                      const RiscvDecodedOperand& operand,
                      uint32& encoding);

// Return the text of a vtype immediate ("e32, m1, ta, ma"), or the number
// when it has a reserved value
cString formatVtype(uint vtype);
// Parse the operands of vsetvli / vsetivli text ("e32, m1, ta, ma"): the
// words of the vtype operand, comma separated. Return false if invalid
bool parseVtype(const cString& text, uint& vtype);

} // namespace riscvdis

#endif // __TBA_DISMOUNT_PROC_RISCV_RISCVOPERANDCODECINTERNAL_H
