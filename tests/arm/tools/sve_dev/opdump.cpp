#include "xStl/types.h"
#include "xStl/stream/memoryStream.h"
#include "dismount/dismount.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char** argv)
{
    char line[256];
    while (fgets(line, sizeof(line), stdin))
    {
        uint32 w = (uint32)strtoul(line, NULL, 16);
        cBuffer bytes; bytes.changeSize(4);
        for (int i = 0; i < 4; i++) bytes[i] = (w >> (8*i)) & 255;
        cMemoryStream stream(bytes);
        ArmStreamDisassembler d(ArmInstructionSet::ARM_A64, BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0x1000), false);
        OpcodePtr op = d.next();
        ArmOpcode* a = (ArmOpcode*)op.getPointer();
        printf("%08x entry=%s n=%lld\n", w, a->getEntry()->m_mnemonic, (long long)a->getOperandsCount());
        for (uint i = 0; i < a->getOperandsCount(); i++)
        {
            const ArmDecodedOperand& o = a->getOperand(i);
            printf("  [%lld] kind=%lld reg=%lld arr=%lld idx=%lld pq=%lld pc=%lld count=%lld stride=%lld imm=%lld fimm=%g shift=%lld amt=%lld base=%lld index=%lld iarr=%lld off=%lld addr=%lld opt=%lld sch=%lld\n",
              (long long)i, (long long)o.m_kind, (long long)o.m_register, (long long)o.m_arrangement, (long long)o.m_elementIndex, (long long)o.m_predicateQualifier, (long long)o.m_isPredicateCounter, (long long)o.m_count, (long long)o.m_stride,
              (long long)o.m_immediate, o.m_floatImmediate, (long long)o.m_shift, (long long)o.m_shiftAmount, (long long)o.m_base, (long long)o.m_index, (long long)o.m_indexArrangement, (long long)o.m_offset, (long long)o.m_addressing, (long long)o.m_option, (long long)o.m_optionScheme);
        }
    }
    return 0;
}
