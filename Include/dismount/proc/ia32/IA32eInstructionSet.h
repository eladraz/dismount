#ifndef __TBA_DISMOUNT_PROC_IA32_IA32EINSTRUCTIONSET_H
#define __TBA_DISMOUNT_PROC_IA32_IA32EINSTRUCTIONSET_H

/*
 * IA32eInstructionSet.h
 *
 * Contains a list of all instruction-set decoder ia32e processor has to offer:
 *    - Intel 16bit x86 instruction set
 *    - Intel 32bit x386 instruction set
 *    - AMD 64bit (AMD64, Intel-XEON, IA32E) instruction set
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"

class IA32eInstructionSet {
public:
    enum DisassemblerTypes {
        // Intel 16bit x86 instruction set
        INTEL_16,
        // Intel 32bit x386 instruction set
        INTEL_32,
        // AMD 64bit (AMD64, Intel-XEON, IA32E) instruction set
        AMD_64
    };
};

#endif // __TBA_DISMOUNT_PROC_IA32_IA32EINSTRUCTIONSET_H
