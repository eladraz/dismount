#ifndef __TBA_DISMOUNT_SEMANTICS_SEMANTICPRINTER_H
#define __TBA_DISMOUNT_SEMANTICS_SEMANTICPRINTER_H

/*
 * SemanticPrinter.h
 *
 * A text dump of a semantic block: its guest instructions and the IR of each
 * of them, for the debugging and for the dumps of the tools (--dump-ir).
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/stream/stringerStream.h"
#include "dismount/semantics/SemanticBlock.h"
#include "dismount/semantics/ArchitectureDescription.h"

/*
 * Print a block as text. The format, one line per IR instruction:
 *
 *     ; guest 0x80001000 size 4
 *     0x80001000: t0:i64 = add x11, x12
 *     0x80001000: x10 = move t0
 *     ; fall-through 0x80001004
 *
 * - Each guest instruction starts with a header line (its address and size).
 *   Its IR lines are prefixed with the guest address, so every line says which
 *   guest instruction it belongs to.
 * - A temporary is printed as tN with its type at its definition (tN:type).
 *   Its uses are printed as tN. A register is printed by its name.
 * - An immediate is printed as a signed decimal, a guest address and a
 *   branch target in hexadecimal, a label as Ln (OP_LABEL prints "Ln:").
 * - A conditional operation (OP_COMPARE, OP_SET_FLAGS...) prints its
 *   condition in brackets: "t2:i1 = compare[COND_SIGNED_LESS] x10, x11".
 * - An operation without a destination prints its type after its name when
 *   the type isn't void: "store:i32 t3, x10".
 * - The operation attributes (SemanticInstruction::getAttributes()) are
 *   printed as "{attributes 0x..}" when they are set.
 *
 * The output is not parsed back: it is meant for humans and the tests.
 */
class SemanticPrinter {
public:
    /*
     * Print a block
     *
     * block        - The block to print
     * architecture - The processor whose registers the block uses, to name them
     * output       - The stream to write the text into
     */
    static void print(const SemanticBlock& block,
                      const ArchitectureDescription& architecture,
                      cStringerStream& output);
};

#endif // __TBA_DISMOUNT_SEMANTICS_SEMANTICPRINTER_H
