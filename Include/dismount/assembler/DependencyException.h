#ifndef __TBA_DISMOUNT_DEPENDECY_EXCEPTION_H
#define __TBA_DISMOUNT_DEPENDECY_EXCEPTION_H

/*
 * DependencyException.h
 *
 * Indicate that the dependncy object isn't large enough for the relocatation
 * Get thron at SecondPassBinary constructor object
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/exceptions.h"
#include "dismount/assembler/BinaryDependencies.h"

class DependencyException : public cException {
public:
    // Default constructor.
    DependencyException(char * file, uint32 line,
                        const BinaryDependencies::DependencyObject& dependency,
                        uint desiredDistance);

    // Return the dependency that caused
    const BinaryDependencies::DependencyObject& getFaultDependency() const;

    // Return the distance relocation size
    uint getDesiredDistance() const;

    // Override the clone function
    virtual cException* clone() const;

    // Override print functions
    virtual const character* getMessage();
    virtual void print();

private:
    BinaryDependencies::DependencyObject m_faultDependency;
    uint m_desiredDistance;
};


#endif // __TBA_DISMOUNT_DEPENDECY_EXCEPTION_H
