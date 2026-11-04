#include "xStl/types.h"
#include "xStl/stream/memoryStream.h"
#include "dismount/dismount.h"
#include "dismount/proc/arm/ArmStreamDisassembler.h"
#include "dismount/proc/arm/ArmOpcode.h"
#include "dismount/proc/arm/ArmSemantics.h"
#include "dismount/semantics/SemanticBlock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void printType(const SemanticType& t)
{
    const char* k = "v";
    switch (t.getKind()) { case SemanticType::KIND_INTEGER: k = "i"; break; case SemanticType::KIND_FLOAT: k = "f"; break;
                           case SemanticType::KIND_MASK: k = "k"; break; default: k = "void"; }
    if (t.getLanes() > 1) printf("%s%lldx%lld", k, (long long)t.getLanes(), (long long)t.getElementBits());
    else printf("%s%lld", k, (long long)t.getElementBits());
}
static void printOp(const SemanticOperand& o)
{
    switch (o.getKind())
    {
    case SemanticOperand::KIND_NONE: return;
    case SemanticOperand::KIND_REGISTER: printf(" r%lld", (long long)o.getIndex()); break;
    case SemanticOperand::KIND_TEMPORARY: printf(" t%lld", (long long)o.getIndex()); break;
    case SemanticOperand::KIND_IMMEDIATE: printf(" #%lld", (long long)o.getValue()); break;
    case SemanticOperand::KIND_LABEL: printf(" L%lld", (long long)o.getIndex()); break;
    default: printf(" @%llx", (long long)o.getValue());
    }
    printf(":"); printType(o.getType());
}
int main(int argc, char** argv)
{
    char line[256];
    int vl = argc > 1 ? atoi(argv[1]) : 128;
    while (fgets(line, sizeof(line), stdin))
    {
        uint32 w = (uint32)strtoul(line, NULL, 16);
        cBuffer bytes; bytes.changeSize(4);
        for (int i = 0; i < 4; i++) bytes[i] = (w >> (8*i)) & 255;
        cMemoryStream stream(bytes);
        ArchitectureDescriptionPtr arch(new Arm64Architecture(vl, 512));
        ArmSemanticLifter lifter(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE, arch);
        ArmStreamDisassembler d(ArmInstructionSet::ARM_A64, BasicInputPtr(&stream, SMARTPTR_DESTRUCT_NONE), true,
            ProcessorAddress(ProcessorAddress::PROCESSOR_64, 0x1000), false);
        OpcodePtr op = d.next();
        SemanticBlock block(OpcodeSubsystems::DISASSEMBLER_ARM_64_LE);
        lifter.lift(op, block);
        printf("== %s\n", line);
        for (cList<SemanticInstruction>::iterator i = block.getInstructions().begin(); i != block.getInstructions().end(); i++)
        {
            const SemanticInstruction& in = *i;
            printf("  ");
            if (in.getDestination().isValid()) { printOp(in.getDestination()); printf(" ="); }
            printf(" %s<", SemanticOperation::getName(in.getOperation()));
            printType(in.getType());
            printf(">");
            for (uint k = 0; k < 3; k++) printOp(in.getSource(k));
            if (in.getOperation() == SemanticOperation::OP_COMPARE) printf(" cond=%d", (int)in.getCondition());
            printf("\n");
        }
    }
    return 0;
}
