#ifndef __TBA_CLR_RUNNABLE_STACK_H
#define __TBA_CLR_RUNNABLE_STACK_H

/*
 * Stack.h
 *
 * Contains function for stack handling
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "xStl/data/list.h"
#include "xStl/data/smartptr.h"

#ifdef XSTL_WINDOWS
// Template classes might not use all the local functions the interface has
// to ofer. Warning C4505 should be over-written for template functions
#pragma warning(push)
#pragma warning(disable:4505)
#endif

/*
 * Stack implementation using link-list
 *
 * NOTE: This class is not thread-safe
 */
template <class T, class Itr>
class StackInrastructor {
public:
    // Default constructor will auto-generated by the compiler

    /*
     * Pop a value from the stack
     *
     * output - Will be filled with the top-of-the stack variable
     *
     * Throw exception if the stack is empty
     */
    void pop(T& output);

    /*
     * Clear the top-of-stack element
     *
     * Throw exception if the stack is empty
     */
    void pop2null();

    /*
     * Peek into the last element in the stack
     */
    const T& peek() const;

    /*
     * Get the top-of-stack element
     */
    T& tos();

    /*
     * Return true if the stack is empty
     */
    bool isEmpty() const;

    /*
     * Push a variable into the stack
     *
     * var - The new variable to be pushed.
     *
     * Throw exception in debug mode if the size of the stack exceed the
     * excpected stack's number of elements
     */
    void push(const T& var);

    /*
     * TODO! Protect these methods
     *
     * Return the stack itself and iterator to TOS. Used in exception-handling
     * for checking the position of top stack for reverting.
     */
    cList<T>& getList();
    const Itr getTosPosition();

    /*
     * Revert the stack into the marker returned by getTosPosition()
     */
    void revertStack(const Itr& pos);

    /*
     * Delete the stack
     */
    void clear();

private:
    // The stack itself.
    cList<T> m_stack;
};

// Forward deceleration
class StackRepEntity;

/*
// Default stack base on Variable
typedef StackInrastructor<Variable, ListOfVariable::iterator> Stack;
// Compiler stack representation
typedef StackInrastructor<StackRepEntity, cList<StackRepEntity>::iterator> StackRep;
// Reference object into the stack
typedef cSmartPtr<Stack> StackPtr;
*/

// And include template implementation
#include "Stack.inl"

#endif // __TBA_CLR_RUNNABLE_STACK_H

