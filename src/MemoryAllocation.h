
#ifndef LOUSE_MEMORYALLOCATION_H
#define LOUSE_MEMORYALLOCATION_H 1

#include <cstdlib>
#include <cstdint>
#include <cstring>

namespace debugging {

////////////////////////////////////////////////////////////////////////////////
/// @brief helper function for rounding up to next multiple of 16
////////////////////////////////////////////////////////////////////////////////

  constexpr size_t roundup (size_t value) {
    return (value + 15) - ((value + 15) & 15);
  }

// -----------------------------------------------------------------------------
// --SECTION--                                                      public types
// -----------------------------------------------------------------------------

  struct MemoryAllocation {

////////////////////////////////////////////////////////////////////////////////
/// @brief memory allocation / deallocation type
////////////////////////////////////////////////////////////////////////////////

    enum AccessType : uint32_t {
      TYPE_INVALID = 0,
      TYPE_NEW,
      TYPE_NEW_ARRAY,
      TYPE_MALLOC,
      TYPE_DELETE,
      TYPE_DELETE_ARRAY,
      TYPE_FREE
    };

// -----------------------------------------------------------------------------
// --SECTION--                                           struct MemoryAllocation
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

    private:

////////////////////////////////////////////////////////////////////////////////
/// @brief create an allocation
////////////////////////////////////////////////////////////////////////////////

      MemoryAllocation () = delete;

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy an allocation
////////////////////////////////////////////////////////////////////////////////

      ~MemoryAllocation () = delete;

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief initialize a memory block
/// this is a replacement for the constructor
////////////////////////////////////////////////////////////////////////////////

      void init (size_t, AccessType);

////////////////////////////////////////////////////////////////////////////////
/// @brief intentionally wipe the memory block's signature
////////////////////////////////////////////////////////////////////////////////

      void wipeSignature () {
        ownSignature = InvalidSignature;     
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the address of the block's memory as returned to the user
////////////////////////////////////////////////////////////////////////////////

      void* memory () {
        return static_cast<void*>(reinterpret_cast<char*>(this) + OwnSize());
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the address of the block's memory as returned to the user
////////////////////////////////////////////////////////////////////////////////

      void const* memory () const {
        return static_cast<void const*>(reinterpret_cast<char const*>(this) + OwnSize());
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the address of the block's tail signature
////////////////////////////////////////////////////////////////////////////////

      void* tailSignatureAddress () {
        return static_cast<char*>(memory()) + size;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief get the address of the block's tail signature
////////////////////////////////////////////////////////////////////////////////

      void const* tailSignatureAddress () const {
        return static_cast<char const*>(memory()) + size;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the memory block is valid
////////////////////////////////////////////////////////////////////////////////

      bool isValid () const {
        return (isOwnSignatureValid() && isTailSignatureValid());
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the memory block's own signature is valid
////////////////////////////////////////////////////////////////////////////////

      bool isOwnSignatureValid () const {
        return ownSignature == ValidSignature;
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the memory block's tail signature is valid
////////////////////////////////////////////////////////////////////////////////

      bool isTailSignatureValid () const {
        return (::memcmp(tailSignatureAddress(), &TailSignature, sizeof(TailSignature)) == 0);
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the own size (overhead) of a memory block
////////////////////////////////////////////////////////////////////////////////

      static size_t OwnSize () {
        return roundup(sizeof(MemoryAllocation));
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the total overhead (own size + tail signature size) of a
/// memory block
////////////////////////////////////////////////////////////////////////////////

      static size_t TotalSize () {
        return OwnSize() + sizeof(TailSignature);
      }

////////////////////////////////////////////////////////////////////////////////
/// @brief return the name for a memory allocation / deallocation method
////////////////////////////////////////////////////////////////////////////////

      static char const* AccessTypeName (AccessType);

////////////////////////////////////////////////////////////////////////////////
/// @brief get the matching deallocation method for an allocation method
////////////////////////////////////////////////////////////////////////////////

      static AccessType MatchingFreeType (AccessType);

// -----------------------------------------------------------------------------
// --SECTION--                                                  public variables
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief size of memory allocated by the user
////////////////////////////////////////////////////////////////////////////////

      size_t            size;

////////////////////////////////////////////////////////////////////////////////
/// @brief stacktrace of the allocation site
////////////////////////////////////////////////////////////////////////////////

      void**            stack;

////////////////////////////////////////////////////////////////////////////////
/// @brief method used for allocating memory 
////////////////////////////////////////////////////////////////////////////////

      AccessType        type;

////////////////////////////////////////////////////////////////////////////////
/// @brief own signature of memory block
/// this is set on allocation and wiped on deallocation
////////////////////////////////////////////////////////////////////////////////

      uint32_t          ownSignature;

////////////////////////////////////////////////////////////////////////////////
/// @brief previous memory block (linked list used by heap)
////////////////////////////////////////////////////////////////////////////////

      MemoryAllocation* prev;

////////////////////////////////////////////////////////////////////////////////
/// @brief next memory block (linked list used by heap)
////////////////////////////////////////////////////////////////////////////////

      MemoryAllocation* next;

// -----------------------------------------------------------------------------
// --SECTION--                                          private static variables
// -----------------------------------------------------------------------------

    private:

////////////////////////////////////////////////////////////////////////////////
/// @brief valid block signature for allocated blocks
////////////////////////////////////////////////////////////////////////////////

      static const uint32_t ValidSignature;

////////////////////////////////////////////////////////////////////////////////
/// @brief invalid block signature for freed blocks
////////////////////////////////////////////////////////////////////////////////

      static const uint32_t InvalidSignature;

////////////////////////////////////////////////////////////////////////////////
/// @brief tail signature for blocks (used to small check buffer overruns)
////////////////////////////////////////////////////////////////////////////////

      static const uint32_t TailSignature;

  };
}

#endif
