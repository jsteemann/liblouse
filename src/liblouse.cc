
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <new>

#include "Tracker.h"

// -----------------------------------------------------------------------------
// --SECTION--                                                 private variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief the global Tracker instance
////////////////////////////////////////////////////////////////////////////////

static debugging::Tracker Tracker;

// -----------------------------------------------------------------------------
// --SECTION--                                            library initialization
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief initializes the library
////////////////////////////////////////////////////////////////////////////////

__attribute__((constructor)) void InitLibrary () { 
  // read the configuration from the environment
  Tracker.Config.fromEnvironment();
}

// -----------------------------------------------------------------------------
// --SECTION--                                         the intercepted functions
// -----------------------------------------------------------------------------
 
////////////////////////////////////////////////////////////////////////////////
/// @brief new
////////////////////////////////////////////////////////////////////////////////

void* operator new (size_t size) throw(std::bad_alloc) {
  void* pointer = Tracker.allocateMemory(size, debugging::MemoryAllocation::TYPE_NEW);

  if (pointer == nullptr) {
    throw std::bad_alloc();
  }

  return pointer;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief new nothrow
////////////////////////////////////////////////////////////////////////////////

void* operator new (size_t size, std::nothrow_t const&) throw() {
  return Tracker.allocateMemory(size, debugging::MemoryAllocation::TYPE_NEW);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief new[]
////////////////////////////////////////////////////////////////////////////////

void* operator new[] (size_t size) throw(std::bad_alloc) {
  void* pointer = Tracker.allocateMemory(size, debugging::MemoryAllocation::TYPE_NEW_ARRAY);

  if (pointer == nullptr) {
    throw std::bad_alloc();
  }

  return pointer;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief new[] nothrow
////////////////////////////////////////////////////////////////////////////////

void* operator new[] (size_t size, std::nothrow_t const&) throw() {
  return Tracker.allocateMemory(size, debugging::MemoryAllocation::TYPE_NEW_ARRAY);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief delete
////////////////////////////////////////////////////////////////////////////////

void operator delete (void* pointer) throw() {
  Tracker.freeMemory(pointer, debugging::MemoryAllocation::TYPE_DELETE);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief delete nothrow
////////////////////////////////////////////////////////////////////////////////

void operator delete (void* pointer, std::nothrow_t const&) throw() {
  Tracker.freeMemory(pointer, debugging::MemoryAllocation::TYPE_DELETE);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief delete[]
////////////////////////////////////////////////////////////////////////////////

void operator delete[] (void* pointer) throw() {
  Tracker.freeMemory(pointer, debugging::MemoryAllocation::TYPE_DELETE_ARRAY);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief delete[] nothrow
////////////////////////////////////////////////////////////////////////////////

void operator delete[] (void* pointer, std::nothrow_t const&) throw() {
  Tracker.freeMemory(pointer, debugging::MemoryAllocation::TYPE_DELETE_ARRAY);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief malloc()
////////////////////////////////////////////////////////////////////////////////

void* malloc (size_t size) {
  // ::fprintf(stderr, "malloc size %lu\n", (unsigned long) size);

  // we don't treat malloc(0) specially here

  if (debugging::Tracker::State == debugging::Tracker::STATE_UNINITIALIZED) {
    debugging::Tracker::Initialize();
  }
 
  void* pointer;
  if (debugging::Tracker::State == debugging::Tracker::STATE_TRACING) {
    pointer = Tracker.allocateMemory(size, debugging::MemoryAllocation::TYPE_MALLOC);
  }
  else {
    pointer = debugging::Tracker::AllocateInitialMemory(size);
  }

  if (pointer == nullptr) {
    errno = ENOMEM;
  }

  return pointer;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief calloc()
////////////////////////////////////////////////////////////////////////////////

void* calloc (size_t nmemb, size_t size) {
  // ::fprintf(stderr, "calloc nmemb %lu size %lu\n", (unsigned long) nmemb, (unsigned long) size);

  // we don't treat calloc(0, x) or calloc(x, 0) specially here

  size_t const totalSize = nmemb * size;

  if (debugging::Tracker::State == debugging::Tracker::STATE_UNINITIALIZED) {
    debugging::Tracker::Initialize();
  }
  
  void* pointer;
  if (debugging::Tracker::State == debugging::Tracker::STATE_TRACING) {
    pointer = Tracker.allocateMemory(totalSize, debugging::MemoryAllocation::TYPE_MALLOC);
  }
  else {
    pointer = debugging::Tracker::AllocateInitialMemory(totalSize);
  }

  if (pointer == nullptr) {
    errno = ENOMEM;
  }
  else {
    memset(pointer, 0, totalSize);
  }

  return pointer;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief realloc()
////////////////////////////////////////////////////////////////////////////////

void* realloc (void* pointer, size_t size) {
  // ::fprintf(stderr, "realloc, pointer %p size %lu\n", pointer, (unsigned long) size);

  if (debugging::Tracker::State == debugging::Tracker::STATE_UNINITIALIZED) {
    debugging::Tracker::Initialize();
  }

  if (pointer == nullptr) {
    // same as malloc(size)
    void* memory;
    if (debugging::Tracker::State == debugging::Tracker::STATE_TRACING) {
      memory = Tracker.allocateMemory(size, debugging::MemoryAllocation::TYPE_MALLOC);
    }
    else {
      memory = debugging::Tracker::AllocateInitialMemory(size);
    }

    if (memory == nullptr) {
      errno = ENOMEM;
    }

    return memory;
  }
    
  size_t oldSize = Tracker.memorySize(pointer);

  if (oldSize >= size) {
    return pointer;
  }

  void* memory = Tracker.allocateMemory(size, debugging::MemoryAllocation::TYPE_MALLOC);

  if (memory == nullptr) {
    errno = ENOMEM;
  }
  else {
    ::memcpy(memory, pointer, oldSize);
    Tracker.freeMemory(pointer, debugging::MemoryAllocation::TYPE_FREE);
  }

  return memory;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free()
////////////////////////////////////////////////////////////////////////////////

void free (void* pointer) {
  // ::fprintf(stderr, "free(%p)\n", pointer);

  if (pointer == nullptr) {
    return;
  }

  if (debugging::Tracker::State == debugging::Tracker::STATE_UNINITIALIZED) {
    debugging::Tracker::Initialize();
  }

  if (debugging::Tracker::State == debugging::Tracker::STATE_TRACING) {
    Tracker.freeMemory(pointer, debugging::MemoryAllocation::TYPE_FREE);
  }
  else{
    debugging::Tracker::FreeInitialMemory(pointer);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief posix_memalign()
/// this function is currently not supported, and calling it will result in
/// program termination
////////////////////////////////////////////////////////////////////////////////

// non-implemented functions
int posix_memalign (void** /*memptr*/, size_t /*alignment*/, size_t /*size*/) {
  debugging::Tracker::ImmediateAbort("assertion", "posix_memalign() is not handled");
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief aligned_alloc()
/// this function is currently not supported, and calling it will result in
/// program termination
////////////////////////////////////////////////////////////////////////////////

void* aligned_alloc (size_t /*alignment*/, size_t /*size*/) {
  debugging::Tracker::ImmediateAbort("assertion", "aligned_alloc() is not handled");
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief exit()
////////////////////////////////////////////////////////////////////////////////

void exit (int status) {
  if (debugging::Tracker::State == debugging::Tracker::STATE_UNINITIALIZED) {
    debugging::Tracker::Initialize();
  }

  if (debugging::Tracker::State == debugging::Tracker::STATE_TRACING) {
    Tracker.finalize();
  }

  debugging::Tracker::Exit(status, false);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief _exit()
////////////////////////////////////////////////////////////////////////////////

void _exit (int status) {
  if (debugging::Tracker::State == debugging::Tracker::STATE_UNINITIALIZED) {
    debugging::Tracker::Initialize();
  }

  if (debugging::Tracker::State == debugging::Tracker::STATE_TRACING) {
    Tracker.finalize();
  }

  debugging::Tracker::Exit(status, true);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief _Exit()
////////////////////////////////////////////////////////////////////////////////

void _Exit (int status) {
  if (debugging::Tracker::State == debugging::Tracker::STATE_UNINITIALIZED) {
    debugging::Tracker::Initialize();
  }

  if (debugging::Tracker::State == debugging::Tracker::STATE_TRACING) {
    Tracker.finalize();
  }

  debugging::Tracker::Exit(status, true);
}

