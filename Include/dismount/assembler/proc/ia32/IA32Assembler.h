#ifndef __TBA_DISMOUNT_ASSEMBLER_PROCESSORS_IA32_IA32ASSEMBLER_H
#define __TBA_DISMOUNT_ASSEMBLER_PROCESSORS_IA32_IA32ASSEMBLER_H

/*
 * IA32Assembler.h
 *
 * Provide an easy and simple way to generate ia32 binary opcodes by assembling
 * simplier english instructions
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/string.h"
#include "xStl/parser/parser.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/IntegerEncoding.h"
#include "dismount/assembler/AssemblerInterface.h"
#include "dismount/assembler/FirstPassBinary.h"
#include "dismount/proc/ia32/IA32OpcodeDatastruct.h"
#include "dismount/proc/ia32/opcodeTable.h"
#include "dismount/proc/ia32/IA32eInstructionSet.h"

/*
 * Implement the AssemblerInterface for IA32 processor family. Including the
 * old 16 bit processor up to the 32 bit processor with MMX/SMID and the 64bit
 * REX extention processors.
 */
class IA32Assembler : public AssemblerInterface {
public:
    /*
     * Constructor
     *
     * binary        - The binary to fill the information to
     * processorMode - The mode of the processor can be either 16 bit, 32 bit
     *                 and more
     */
    IA32Assembler(FirstPassBinaryPtr binary,
                  IA32eInstructionSet::DisassemblerTypes processorMode);

    // See AssemblerInterface::getAssembler()
    virtual cStringerStreamPtr getAssembler();
    // See AssemblerInterface::getFirstPass()
    virtual FirstPassBinaryPtr getFirstPass();

private:
    // The binary object
    FirstPassBinaryPtr m_binary;
    // The processor mode
    IA32eInstructionSet::DisassemblerTypes m_processorMode;
};

/*
 * Usage:
 *     IA32Assembler compiler(firstBinaryPass, INTEL_32);
 *     compiler << "push ebp" << endl;
 *     compiler << "mov  ebp, esp" << endl;
 *     compiler << "sub  esp, 4" << endl;
 *      ...
 *     compiler << "mov  esp, ebp" << endl;
 *     compiler << "pop  ebp" << endl;
 *     compiler << "ret" << endl;
 *
 * TODO! Add to this assumption dependency mode.
 *   When the destructor is called all the source code will be assemble and
 *   append to the binary code.
 *
 * When the compiler seeing a label he don't even trying to resolve it. The name
 * of the label is added into the dependency tree and will be resolved later;
 * The binary will contains an empty reference.
 *
 * Opcodes such as 'call eax' will not be appended to the relocation assuming
 * that the 'mov' opcode causes the relocation.
 */
class IA32AssemblerStream : public cStringStream {
public:
    /*
     * Constructor
     *
     * binary - The binary to fill the information to
     * processorMode - The mode of the processor can be either 16 bit, 32 bit
     *                 and more
     */
    IA32AssemblerStream(FirstPassBinary& binary,
                        IA32eInstructionSet::DisassemblerTypes processorMode);

    /*
     * Destructor. Compile the stream
     */
    virtual ~IA32AssemblerStream();

private:
    /*
     * Represent an operand
     *
     * Author: Elad Raz
     */
    class Operand {
    public:
        // Default constructor. Define that this operand is not in use
        Operand();

        // Copy-constructor and operator = will auto-generated by the compiler

        /*
         * Read an operand from opcode.
         *
         * NOTE: Reading operands is only allowed when class is empty
         *       (Which means that there isn't any previous information inside
         *        class's memory)
         *
         * parser - The parser object
         *
         * Throw exception if the operand format is invalid
         */
        void readOperand(Parser& parser);

        // List of all operand types
        enum Type {
            // This operand is empty
            NO_OPERAND,

            //
            // O TTTT  MM II SRR
            // |    |   |  |  +-->  Registry mask
            // |    |   |  +----->  Immediate size mask
            // |    |   +-------->  Memory access mask
            // |    +------------>  Memory access mask
            // +----------------->  Offset bit

            // Register mask ((type & OPERAND_REGISTRY_MASK) != 0)
            OPERAND_REGISTRY_MASKSHL = 3,
            OPERAND_REGISTRY_MASK = ((1 << OPERAND_REGISTRY_MASKSHL) - 1),
                // The operand is 8 bit register
                OPERAND_8BIT_REG  = (1 << 0),
                // The operand is 16 bit register
                OPERAND_16BIT_REG = (2 << 0),
                // The operand is 32 bit register
                OPERAND_32BIT_REG = (3 << 0),
                // The operand is one of the segments registers (cs, es...)
                OPERAND_SEGMENT_REG = (4 << 0),
                // The operand is a XMM register
                OPERAND_XMM_REG = (5 << 0),
                // The operand is a SIMD register
                OPERAND_SIMD_REG = (6 << 0),


            // Immediate
            OPERAND_IMMEDIATE_MASKSHL = (OPERAND_REGISTRY_MASKSHL + 2),
            OPERAND_IMMEDIATE_MASK = (((1 << 2) - 1) << OPERAND_REGISTRY_MASKSHL),
                // The operand is 8 bit const value
                OPERAND_IMMEDIATE_8BIT  = (1 << OPERAND_REGISTRY_MASKSHL),
                // The operand is 16 bit const value
                OPERAND_IMMEDIATE_16BIT = (2 << OPERAND_REGISTRY_MASKSHL),
                // The operand is 32 bit const value
                OPERAND_IMMEDIATE_32BIT = (3 << OPERAND_REGISTRY_MASKSHL),

            // Memory
            OPERAND_MEMORY_ACCESS_MASKSHL = (OPERAND_IMMEDIATE_MASKSHL + 2),
            OPERAND_MEMORY_ACCESS_MASK = (((1 << 2) - 1) << OPERAND_IMMEDIATE_MASKSHL),
                // Memory access for 8 bit
                OPERAND_MEMORY_8BIT  = (1 << OPERAND_IMMEDIATE_MASKSHL),
                // Memory access for 16 bit
                OPERAND_MEMORY_16BIT = (2 << OPERAND_IMMEDIATE_MASKSHL),
                // Memory access for 32 bit
                OPERAND_MEMORY_32BIT = (3 << OPERAND_IMMEDIATE_MASKSHL),

            // Memory access make
            OPERAND_MEMORY_TYPE_MASKSHL = (OPERAND_MEMORY_ACCESS_MASKSHL + 4),
            OPERAND_MEMORY_TYPE_MASK    = (((1 << 4) - 1) << OPERAND_MEMORY_ACCESS_MASKSHL),
                OPERAND_MEMORY_DIRECT       = (1 << OPERAND_MEMORY_ACCESS_MASKSHL),
                OPERAND_MEMORY_REG          = (2 << OPERAND_MEMORY_ACCESS_MASKSHL),
                OPERAND_MEMORY_REG_PLUS_REG = (3 << OPERAND_MEMORY_ACCESS_MASKSHL),
                OPERAND_MEMORY_REG_PLUS_DISPLACEMENT = (4 << OPERAND_MEMORY_ACCESS_MASKSHL),
                OPERAND_MEMORY_REG_PLUS_REG_PLUS_DISPLACEMENT = (5 << OPERAND_MEMORY_ACCESS_MASKSHL),

            // Relative/absolute offset bit
            OPERAND_OFFSET_MASK = (1 << OPERAND_MEMORY_TYPE_MASKSHL)
        };

        /*
         * Return the type of the operand
         */
        uint getType() const;

        /*
         * Return the first reference for the operand.
         * The reference depends on the type:
         *    Registers (8/16/32): The register index (0 to 7)
         *    Immediate (8/16/32): The immediate value limited to operand size
         *    Direct memory:       The displacement
         *    Memory with reg1:    The first registry index
         *    Offset:              N/A
         */
        uint32 getReference1() const;

        /*
         * Return the second reference for the operand
         * The reference depends on the type:
         *    Registers (8/16/32): N/A
         *    Immediate (8/16/32): N/A
         *    Direct memory:       N/A
         *    Offset:              N/A
         */
        //uint32 getReference2() const;

        /*
         * Return the displacement information and best encoding method, or the
         * relative offset location
         */
        int32 getDisplacement() const;
        IntegerEncoding::IntegerEncodingType getDisplacementPacking() const;

    private:
        /*
         * Terminate the operand reading process
         * Throw exception if unexpected information is giving
         */
        static void readUntilOpcode(Parser& parser);

        /*
         * Check a word to be a register (al/ah/ax/eax...)
         *
         * regname  - The name of the register
         * regType  - Will be filled with the registry type. See Operand::Type
         * regIndex - The index of the registry inside the reg-table
         *
         * Return true if 'name' is save register name
         */
        static bool checkRegister(const   cString& regname,
                                  uint&   regType,
                                  uint32& regIndex);

        /*
         * According to the 'm_reference1' value decide which of the 8/16/32
         * immediate byte should the operand be
         */
        void limitedImmediate();
        void limitedDisplacement();

        // The operand type. See Operand::Type
        uint m_type;
        // Reference #1 (Depends on operand type). See getReference1()
        uint32 m_reference1;
        // Reference #2 (Depends on operand type). See getReference2()
        // uint32 m_reference2;

        // Displacement
        int32 m_displacement;
        IntegerEncoding::IntegerEncodingType m_displacementEncodingType;
    };

    // The function for the modrm which is not in use.
    enum { MODRM_NOT_EXIST = 4 };

    /*
     * Try to find opcode according to it's name and it's parameters and
     * write it into the first-binary pass
     *
     * name                 - The name of the opcode (e.g. 'mov')
     * first, second, third - The operand description (e.g. OPERAND_32BIT_REG)
     *
     * Throw exception if the opcode is invalid
     */
    void writeOpcode(const cString& name,
                     const Operand& first,
                     const Operand& second,
                     const Operand& third);

    /*
     * Match an opcode into entry of opcode table
     *
     * opcode - The opcode name
     * first, second, third - The operand description (e.g. OPERAND_32BIT_REG)
     * operandSizePrefix - Will be set to true if the operation requires an
     *                     operand size prefix change.
     *                     Set to false if no operand size is required.
     * addressSizePrefix - Will be set to true if the operation requires an
     *                     address size prefix change.
     *                     Set to false if no address size of required.
     * modrm             - Will be set if the modrm is in use, false otherwise
     * modrmMod          - The mod for the selected modrm
     *
     * Return true if the opcode is match
     * Return false otherwise
     */
    bool matchOpcode(const ia32dis::OpcodeEntry* opcode,
                     const Operand& first,
                     const Operand& second,
                     const Operand& third,
                     bool& operandSizePrefix,
                     bool& addressSizePrefix,
                     bool& modrm,
                     IA32OpcodeDatastruct::MODRM& modrmMod) const;

    /*
     * Tries to match between compiler operand information and assembler table
     * operand type.
     *
     * operandSizePrefix - Will be set to true if the operation requires an
     *                     operand size prefix change
     * addressSizePrefix - Will be set to true if the operation requires an
     *                     address size prefix change
     * modrm             - Will be set if the modrm is in use
     * modrmMod          - The mod for the selected modrm
     *
     * NOTE: The addressSizePrefix, operandSizePrefix, modrm and modrmMod will
     *       not set to false!
     *
     * Return true for any possible match
     * False otherwise
     */
    bool matchOperand(ia32dis::OperandType ia32disType,
                       const Operand& compilerOperand,
                       bool& operandSizePrefix,
                       bool& addressSizePrefix,
                       bool& modrm,
                       IA32OpcodeDatastruct::MODRM& modrmMod,
                       bool isUnsigedAllowed) const;

    /*
     * Updates the operand size according to the modrm behaviour.
     * For example, if the current running mode is 32 bit and the opcode access
     * a 16 bit memory reference, then the operandSizePrefix will be set.
     *
     * memorySize        - See Opernad::OPERAND_MEMORY_ACCESS_MASK
     * ia32disType       - Operand access routine
     * operandSizePrefix - The prefix code
     *
     * Return false if the operand-type and memory access are invalid
     */
    bool updateOperandSize(uint memorySize,
                           ia32dis::OperandType ia32disType,
                           bool& operandSizePrefix) const;

    /*
     * Gives an estimation about the size of the opcode
     *
     * tableIndex - The table of which the operand lays on
     * opcode     - The opcode to be check
     * operandSizePrefix - True adds an operand-size prefix into the opcode.
     * addressSizePrefix - True adds an address-size prefix into the opcode.
     * modrm             - True if the ModRM exist
     * modrmMod          - The mod for the modrm, used for SIB calculation.
     *
     * Return the size of the opcode inside the binary
     */
    uint calculateOpcodeSize(uint tableIndex,
                             const ia32dis::OpcodeEntry* opcode,
                             bool operandSizePrefix,
                             bool addressSizePrefix,
                             bool modrm,
                             IA32OpcodeDatastruct::MODRM modrmMod) const;

    /*
     * According to the internal mode of the processor and the opcode operand
     * compilation returns the size of the operand.
     *
     * processorOperandSize - The mode of the processor regarding operand size
     * processorAddressSize - The mode of the processor regarding address size
     * ia32disType          - Opcode operand type
     *
     * Return the immediate size for the operand
     */
    static IntegerEncoding::IntegerEncodingType getOperandSize(
        IntegerEncoding::IntegerEncodingType processorOperandSize,
        IntegerEncoding::IntegerEncodingType processorAddressSize,
        ia32dis::OperandType ia32disType);

    /*
     * From the current processor mode and the operand-size/address-size prefix
     * calculate the size of the encoded operation
     */
    IntegerEncoding::IntegerEncodingType getProcessorOperandSize(
                                                        bool sizePrefix) const;

    /*
     * Encode an opcode inside the first-binary pass
     *
     * table - The table index used for writing table prefix
     * opcode - The opcode encoding format
     * first, second, third - The operand description (e.g. OPERAND_32BIT_REG)
     * operandSizePrefix - True adds an operand-size prefix into the opcode.
     * addressSizePrefix - True adds an address-size prefix into the opcode.
     * modrm             - True if the modrm exist
     * modrmMod          - The mod for the modrm
     *
     * Throw exception if some error occured.
     */
    void encodeOpcode(uint table,
                      const ia32dis::OpcodeEntry* opcode,
                      const Operand& first,
                      const Operand& second,
                      const Operand& third,
                      bool operandSizePrefix,
                      bool addressSizePrefix,
                      bool modrm,
                      IA32OpcodeDatastruct::MODRM modrmMod);

    /*
     * Return the build modrm.
     *
     * modrmMod             - The mod index for the modrm
     * opcode               - The opcode format
     * first, second, third - The operands for the opcode
     */
    IA32OpcodeDatastruct::MODRM fillModrm(IA32OpcodeDatastruct::MODRM modrmMod,
                const ia32dis::OpcodeEntry* opcode,
                const Operand& first,
                const Operand& second,
                const Operand& third);

    /*
     * Fills the modrm R/M for a reference opcode
     *
     * modrm             - Takes the 'mod' part of the ModRM and according to
     *                     the register type and reference fills the r/m part
     * operandSizePrefix - The operand size prefix
     * regType           - The registry encoding type, used for choosing the
     *                     modrm table. See Operand::Type
     * registerReference - The register index
     *
     * Throw exception if the register number if not compatible.
     */
    void fillModrmRM(IA32OpcodeDatastruct::MODRM& modrm,
                     bool operandSizePrefix,
                     uint regType,
                     uint registerReference) const;

    /*
     * From the opcode and the operands, choses the RM operand
     */
    const Operand& getRMOpcode(const ia32dis::OpcodeEntry* opcode,
                               const Operand& first,
                               const Operand& second,
                               const Operand& third);

    /*
     * Encode an immediate inside the binary stream for a single operand.
     *
     * is32OperandSize - Set to true if the operand size is 32 bit size
     * is32AddressSize - Set to true if the address size if 32 bit size
     * ia32disType     - The opcode operand type
     * compilerOperand - The user information
     */
    void writeImmediate(IntegerEncoding::IntegerEncodingType processorOperandSize,
                        IntegerEncoding::IntegerEncodingType processorAddressSize,
                        ia32dis::OperandType ia32disType,
                        const Operand& compilerOperand);

    /*
     * Write a displacement (if any) into the binary
     */
    void writeDisplacement(const Operand& compilerOperand,
                           uint displacementSize);

    /*
     * Adding uint16 or uint32. G
     * Guaranty little endian level.
     */
    void writeUint16(uint16 data);
    void writeUint32(uint32 data);

    /*
     * Return true if the operand is a 32 or a 16 bit. Determine by the
     * processor mode and the current operandPrefix
     */
    bool is32operand(bool operandPrefix) const;
    bool is16operand(bool operandPrefix) const;

    // Return true if value is in range of -0x80..0x7F, return false otherwise
    static bool checkSignedImmediateSize8(int32 value);
    // Return true if value is in range of -0x8000..0x7FFF, return false otherwise
    static bool checkSignedImmediateSize16(int32 value);

    // The binary
    FirstPassBinary& m_binary;
    // The mode of the processor
    IA32eInstructionSet::DisassemblerTypes m_processorMode;
};

#endif // __TBA_DISMOUNT_ASSEMBLER_PROCESSORS_IA32_IA32ASSEMBLER_H

