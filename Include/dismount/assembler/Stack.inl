/*
 * Stack.cpp
 *
 * Implementation file
 *
 * Author: Elad Raz <e@eladraz.com>
 */
#include "xStl/types.h"
#include "dismount/assembler/Stack.h"

class DismountStackError : public cException {
public:
    // Default constructor.
    DismountStackError(char * file, uint32 line) : cException(file, line, NULL, 0)  {};
};

template <class T, class Itr>
void StackInrastructor<T, Itr>::pop(T& output)
{
    if (isEmpty())
        XSTL_THROW(DismountStackError);

    // Get variable
    output = *m_stack.begin();

    // And remove it from the stack (gcc doesn't allow inline reference)
    Itr b(m_stack.begin());
    m_stack.remove(b);
}

template <class T, class Itr>
void StackInrastructor<T, Itr>::pop2null()
{
    if (isEmpty())
        XSTL_THROW(DismountStackError);
    m_stack.remove(m_stack.begin());
}

template <class T, class Itr>
void StackInrastructor<T, Itr>::push(const T& var)
{
    m_stack.insert(var);
}

template <class T, class Itr>
const T& StackInrastructor<T, Itr>::peek() const
{
    if (isEmpty())
        XSTL_THROW(DismountStackError);

    return *m_stack.begin();
}

template <class T, class Itr>
T& StackInrastructor<T, Itr>::tos()
{
    if (isEmpty())
        XSTL_THROW(DismountStackError);

    return *m_stack.begin();
}

template <class T, class Itr>
bool StackInrastructor<T, Itr>::isEmpty() const
{
    return m_stack.begin() == m_stack.end();
}

template <class T, class Itr>
cList<T>& StackInrastructor<T, Itr>::getList()
{
    return m_stack;
}

template <class T, class Itr>
const Itr StackInrastructor<T, Itr>::getTosPosition()
{
    return m_stack.begin();
}

template <class T, class Itr>
void StackInrastructor<T, Itr>::revertStack(const Itr& pos)
{
    while (m_stack.begin() != pos)
    {
        if (isEmpty())
        {
            // Error with stack reverting
            CHECK_FAIL();
        }
        m_stack.remove(m_stack.begin());
    }
}

template <class T, class Itr>
void StackInrastructor<T, Itr>::clear()
{
    m_stack.removeAll();
}
