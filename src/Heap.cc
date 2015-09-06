
#include "Heap.h"
#include "MemoryAllocation.h"

using Heap = debugging::Heap;
using MemoryAllocation = debugging::MemoryAllocation;

// -----------------------------------------------------------------------------
// --SECTION--                                                        class Heap
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create the heap
////////////////////////////////////////////////////////////////////////////////

Heap::Heap ()
  : lock_(), head_(nullptr), numAllocations_(0), sizeAllocations_(0) {
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the heap
////////////////////////////////////////////////////////////////////////////////

Heap::~Heap () {
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief add a memory block to the heap
////////////////////////////////////////////////////////////////////////////////

void Heap::add (MemoryAllocation* allocation) {
  std::lock_guard<std::mutex> locker(lock_);

  allocation->prev = nullptr;
  allocation->next = head_;

  if (head_ != nullptr) {
    head_->prev = allocation;
  }
  head_ = allocation;

  ++numAllocations_;
  sizeAllocations_ += allocation->size;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief remove a memory block from the heap
////////////////////////////////////////////////////////////////////////////////

void Heap::remove (MemoryAllocation* allocation) {
  std::lock_guard<std::mutex> locker(lock_);

  if (allocation->prev != nullptr) {
    allocation->prev->next = allocation->next;
  }

  if (allocation->next != nullptr) {
    allocation->next->prev = allocation->prev;
  }

  if (head_ == allocation) {
    head_ = allocation->next;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not the heap is corrupted
////////////////////////////////////////////////////////////////////////////////

bool Heap::isCorrupted (MemoryAllocation const* start) {
  auto allocation = start;

  while (allocation != nullptr) {
    if (! allocation->isOwnSignatureValid()) {
      return true;
    }

    allocation = allocation->next;
  }

  return false;
}

