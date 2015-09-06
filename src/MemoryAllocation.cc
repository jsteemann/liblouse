
#include "MemoryAllocation.h"
  
using MemoryAllocation = debugging::MemoryAllocation;

// -----------------------------------------------------------------------------
// --SECTION--                                           struct MemoryAllocation
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief initialize a memory block
/// this is a replacement for the constructor
////////////////////////////////////////////////////////////////////////////////

void MemoryAllocation::init (size_t size, MemoryAllocation::AccessType type) {
  this->size         = size;
  this->stack        = nullptr;
  this->ownSignature = MemoryAllocation::ValidSignature;
  this->type         = type;
  this->prev         = nullptr;
  this->next         = nullptr;

  ::memcpy(tailSignatureAddress(), &TailSignature, sizeof(TailSignature));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief return the name for a memory allocation / deallocation method
////////////////////////////////////////////////////////////////////////////////

char const* MemoryAllocation::AccessTypeName (MemoryAllocation::AccessType type) {
  switch (type) {
    case MemoryAllocation::TYPE_NEW:          return "new";
    case MemoryAllocation::TYPE_NEW_ARRAY:    return "new[]";
    case MemoryAllocation::TYPE_MALLOC:       return "malloc()";
    case MemoryAllocation::TYPE_DELETE:       return "delete";
    case MemoryAllocation::TYPE_DELETE_ARRAY: return "delete[]";
    case MemoryAllocation::TYPE_FREE:         return "free()";
    default:                return "invalid"; 
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get the matching deallocation method for an allocation method
////////////////////////////////////////////////////////////////////////////////

MemoryAllocation::AccessType MemoryAllocation::MatchingFreeType (MemoryAllocation::AccessType type) {
  switch (type) {
    case MemoryAllocation::TYPE_NEW:          return MemoryAllocation::TYPE_DELETE;
    case MemoryAllocation::TYPE_NEW_ARRAY:    return MemoryAllocation::TYPE_DELETE_ARRAY;
    case MemoryAllocation::TYPE_MALLOC:       return MemoryAllocation::TYPE_FREE;
    default:                                  return MemoryAllocation::TYPE_INVALID;
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                          private static variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief valid block signature for allocated blocks
////////////////////////////////////////////////////////////////////////////////

uint32_t const MemoryAllocation::ValidSignature   = 0xdeadcafe;

////////////////////////////////////////////////////////////////////////////////
/// @brief invalid block signature for freed blocks
////////////////////////////////////////////////////////////////////////////////

uint32_t const MemoryAllocation::TailSignature    = 0xdeadbeef;

////////////////////////////////////////////////////////////////////////////////
/// @brief tail signature for blocks (used to small check buffer overruns)
////////////////////////////////////////////////////////////////////////////////

uint32_t const MemoryAllocation::InvalidSignature = 0xbaadc0de;

