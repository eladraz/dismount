#include "dismount/dismount.h"
/*
* FlowMapperException.cpp
*
 * Implementation file
*
* Author: Tal Harel
*/

#include "dismount/FlowMapperException.h"

FlowMapperException::FlowMapperException(char * file, uint32 line) : cException(file, line, NULL, 0)
{
}
