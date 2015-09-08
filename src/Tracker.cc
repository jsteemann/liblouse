
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <unistd.h>
#include <dlfcn.h>
#include <fcntl.h>

#include "Tracker.h"
#include "StackResolver.h"
#include "Printer.h"

using Configuration     = debugging::Configuration;
using MemoryAllocation  = debugging::MemoryAllocation;
using Printer           = debugging::Printer;
using StackResolver     = debugging::StackResolver;
using Tracker           = debugging::Tracker;

// -----------------------------------------------------------------------------
// --SECTION--                                          private helper functions
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief hashes a string value
////////////////////////////////////////////////////////////////////////////////

static uint64_t HashString (char const* buffer) {
  static uint64_t const MagicPrime = 0x00000100000001b3ULL;
  uint64_t hash = 0xcbf29ce484222325ULL;
  uint8_t const* p = (uint8_t const*) buffer;

  while (*p) {
    hash ^= *p++;
    hash *= MagicPrime;
  }

  return hash;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief startup replacement for malloc()
////////////////////////////////////////////////////////////////////////////////

static void* NullMalloc (size_t) {
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief startup replacement for calloc()
////////////////////////////////////////////////////////////////////////////////

static void* NullCalloc (size_t, size_t) {
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief startup replacement for realloc()
////////////////////////////////////////////////////////////////////////////////

static void* NullRealloc (void*, size_t) {
  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief gets a pointer to a library function
////////////////////////////////////////////////////////////////////////////////
      
template<typename T> static T GetLibraryFunction (char const* name) {
  return reinterpret_cast<T>(::dlsym(RTLD_NEXT, name));
}

// -----------------------------------------------------------------------------
// --SECTION--                                                     class Tracker
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create the tracker
/// note: this may be called AFTER the first malloc/calloc!
////////////////////////////////////////////////////////////////////////////////

Tracker::Tracker ()
  : heap_() {

  Initialize();
  State = STATE_TRACING;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the tracker
/// this performs leak checking
////////////////////////////////////////////////////////////////////////////////

Tracker::~Tracker () {
  finalize();
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief staticially initialize the tracker
/// this may be called by the constructor or before
////////////////////////////////////////////////////////////////////////////////
      
void Tracker::Initialize () {
  if (State == STATE_UNINITIALIZED) {
    State = STATE_INITIALIZING;

    LibraryMalloc  = NullMalloc;
    LibraryCalloc  = NullCalloc;
    LibraryRealloc = NullRealloc;

    auto malloc = GetLibraryFunction<MallocFuncType>("malloc");

    if (malloc == nullptr) {
      ImmediateAbort("init", "cannot find malloc()");
    }

    auto calloc = GetLibraryFunction<CallocFuncType>("calloc");

    if (calloc == nullptr) {
      ImmediateAbort("init", "cannot find calloc()");
    }

    auto realloc = GetLibraryFunction<ReallocFuncType>("realloc");

    if (realloc == nullptr) {
      ImmediateAbort("init", "cannot find realloc()");
    }

    auto free = GetLibraryFunction<FreeFuncType>("free");

    if (free == nullptr) {
      ImmediateAbort("init", "cannot find free()");
    }

    auto exit = GetLibraryFunction<ExitFuncType>("exit");

    if (exit == nullptr) {
      ImmediateAbort("init", "cannot find exit()");
    }
    
    auto _exit = GetLibraryFunction<ExitFuncType>("_exit");

    if (_exit == nullptr) {
      ImmediateAbort("init", "cannot find _exit()");
    }

    LibraryMalloc  = malloc;
    LibraryCalloc  = calloc;
    LibraryRealloc = realloc;
    LibraryFree    = free;
    LibraryExit    = exit;
    Library_Exit   = _exit;

    State = STATE_HOOKED;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief terminate the execution
////////////////////////////////////////////////////////////////////////////////

void Tracker::Exit (int status, bool immediately) {
  if (immediately) {
    Library_Exit(status);
    // if LibraryExit() does not work, we must abort anyway
    std::abort();
  }
  else {
    LibraryExit(status);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief print an error and abort execution
////////////////////////////////////////////////////////////////////////////////

void Tracker::ImmediateAbort (char const* type, char const* message) {
  Printer::EmitError(OutFile, type, "%s", message);
  std::abort();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief allocate some untracked memory
////////////////////////////////////////////////////////////////////////////////

void* Tracker::AllocateInitialMemory (size_t size) { 
  if (UntrackedPointersLength == sizeof(UntrackedPointers) / sizeof(UntrackedPointers[0])) {
    ImmediateAbort("allocation", "malloc: out of initialization memory\n");
  }

  void* pointer = LibraryMalloc(size + sizeof(size_t));

  if (pointer != nullptr) {
    size_t* memory = static_cast<size_t*>(pointer);
    *memory = size;
    ++memory;

    pointer = static_cast<void*>(memory);
    UntrackedPointers[UntrackedPointersLength++] = pointer;
  }

  return pointer;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free some untracked memory
////////////////////////////////////////////////////////////////////////////////

bool Tracker::FreeInitialMemory (void* pointer) {
  for (size_t i = 0; i < UntrackedPointersLength; ++i) {
    if (UntrackedPointers[i] == pointer) {
      size_t* memory = static_cast<size_t*>(pointer);
      --memory;
      debugging::Tracker::LibraryFree(static_cast<void*>(memory));
      --UntrackedPointersLength;

      for (size_t j = i; j < UntrackedPointersLength; ++j) {
        UntrackedPointers[j] = UntrackedPointers[j + 1];
      }
      return true;
    }
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief allocate memory that is going to be tracked
////////////////////////////////////////////////////////////////////////////////

void* Tracker::allocateMemory (size_t size, MemoryAllocation::AccessType type) {
  // ::fprintf(stderr, "allocate memory called, size: %lu\n", (unsigned long) size);
  size_t const actualSize = size + MemoryAllocation::TotalSize();
  void* pointer = LibraryMalloc(actualSize);

  if (pointer == nullptr || State != STATE_TRACING) {
    // ::fprintf(stderr, "allocate returning pointer %p\n", pointer);
    return pointer;
  }

  auto allocation = static_cast<MemoryAllocation*>(pointer);

  allocation->init(size, type);

  if (Config.withTraces) { 
    allocation->stack = StackResolver::captureStackTrace(Config.maxFrames);
  }

  heap_.add(allocation);

  // ::fprintf(stderr, "allocate returning wrapped pointer %p, orig: %p\n", allocation->memory(), pointer);
  return allocation->memory();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief free tracked memory
/// this will already report errors
////////////////////////////////////////////////////////////////////////////////

void Tracker::freeMemory (void* pointer, MemoryAllocation::AccessType type) {
  // ::fprintf(stderr, "freeMemory called with %p\n", pointer);
  if (pointer == nullptr) {
    return; 
  }

  if (UntrackedPointersLength > 0) {
    if (FreeInitialMemory(pointer)) {
      return;
    }
  }

  if (State != STATE_TRACING) {
    // ::fprintf(stderr, "call free with %p\n", pointer);
    LibraryFree(pointer);
    return;
  }

  void* mem = static_cast<void*>(static_cast<char*>(pointer) - MemoryAllocation::OwnSize());

  auto allocation = static_cast<MemoryAllocation*>(mem);

  if (! allocation->isOwnSignatureValid()) {
    Printer::EmitError(OutFile,
                       "runtime",
                       "%s called with invalid memory pointer %p", 
                       MemoryAllocation::AccessTypeName(type),
                       pointer);

    emitStackTrace();
  }
  else {
    if (type != MemoryAllocation::MatchingFreeType(allocation->type)) {
      Printer::EmitError(OutFile,
                         "runtime",
                         "trying to %s memory pointer %p that was originally allocated via %s",
                         MemoryAllocation::AccessTypeName(type),
                         pointer,
                         MemoryAllocation::AccessTypeName(allocation->type));

      emitStackTrace();

      if (allocation->stack != nullptr) {
        Printer::EmitLine(OutFile, "");
        Printer::EmitLine(OutFile,
                          "original allocation site of memory pointer %p via %s:",
                          pointer,
                          MemoryAllocation::AccessTypeName(allocation->type));

        emitStackTrace(allocation->stack);
      }
    }

    if (! allocation->isTailSignatureValid()) {
      Printer::EmitError(OutFile,
                         "runtime",
                         "buffer overrun after memory pointer %p of size %llu that was originally allocated via %s",
                         pointer,
                         static_cast<unsigned long long>(allocation->size),
                         MemoryAllocation::AccessTypeName(allocation->type));

      emitStackTrace();

      if (allocation->stack != nullptr) {
        Printer::EmitLine(OutFile, "");
        Printer::EmitLine(OutFile,
                          "original allocation site of memory pointer %p via %s:",
                          pointer,
                          MemoryAllocation::AccessTypeName(allocation->type));

        emitStackTrace(allocation->stack);
      }
    }
  }

  heap_.remove(allocation);

  allocation->wipeSignature();

  if (allocation->stack != nullptr) {
    LibraryFree(allocation->stack);
  }

  LibraryFree(mem);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief get the size of a memory allocation
////////////////////////////////////////////////////////////////////////////////
 
size_t Tracker::memorySize (void* pointer) const {
  for (size_t i = 0; i < UntrackedPointersLength; ++i) {
    if (UntrackedPointers[i] == pointer) {
      size_t* memory = static_cast<size_t*>(pointer);
      --memory;
      return *memory;
    }
  }

  void* memory = static_cast<void*>(static_cast<char*>(pointer) - MemoryAllocation::OwnSize());
  auto allocation = static_cast<MemoryAllocation*>(memory);

  if (allocation->isOwnSignatureValid()) {
    return allocation->size;
  }

  // unknown pointer!
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief finalize the tracker, show results
////////////////////////////////////////////////////////////////////////////////

void Tracker::finalize () {
  if (Finalized) {
    return;
  }

  Finalized = true;
 
  regex_t re;

  if (Config.suppressFilter != nullptr &&
      *Config.suppressFilter != '\0') {
    if (::regcomp(&re, Config.suppressFilter, REG_NOSUB | REG_EXTENDED) != 0) {
      Config.suppressFilter = nullptr;
    }
  }

  try {
    emitResults(&re);
  }
  catch (...) {
  }

  if (Config.suppressFilter != nullptr && 
      *Config.suppressFilter != '\0') {
    ::regfree(&re);
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a leak should be suppressed
////////////////////////////////////////////////////////////////////////////////

bool Tracker::mustSuppressLeak (char const* stack, regex_t* regex) {
  if (Config.suppressFilter == nullptr || 
      *Config.suppressFilter == '\0' ||
      stack == nullptr) {
    return false;
  }

  if (::regexec(regex, stack, 0, nullptr, 0) != 0) {
    return false;
  }

  return true;
}
    
////////////////////////////////////////////////////////////////////////////////
/// @brief prints the current stacktrace
////////////////////////////////////////////////////////////////////////////////

void Tracker::emitStackTrace () {
  void* stack[64];

  if (! StackResolver::captureStackTrace(Config.maxFrames, &stack[0], sizeof(stack) / sizeof(stack[0]))) {
    return;
  }

  emitStackTrace(&stack[0]);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief prints the stacktrace for the stack argument
////////////////////////////////////////////////////////////////////////////////

void Tracker::emitStackTrace (void** stack) {
  if (stack == nullptr) {
    return;
  }

  StackResolver resolver;

  char memory[4096];

  char* buffer = resolver.resolveStack(Config.maxFrames, 
                                       Printer::UseColors(OutFile), 
                                       &memory[0], 
                                       sizeof(memory), 
                                       stack);

  if (buffer != nullptr) {
    Printer::EmitLine(OutFile, "%s", buffer);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief print overall results for the linked list of memory blocks,
/// starting with the argument
////////////////////////////////////////////////////////////////////////////////

void Tracker::emitResults (regex_t* regex) {
  // rebind to tty if printing to OutFile is not possible 
  if (::fprintf(OutFile, "%s", "") < 0) {
    OutFile = ::fopen("/dev/tty", "w");
  }

  Printer::EmitLine(OutFile, "");
  Printer::EmitLine(OutFile, "RESULTS --------------------------------------------------------");
  Printer::EmitLine(OutFile, "");

  auto begin = heap_.begin(); // save current head of heap!
  auto stats = heap_.totals();

  Printer::EmitLine(OutFile,
                    "# total number of allocations: %llu",
                    static_cast<unsigned long long>(stats.first));

  Printer::EmitLine(OutFile,
                    "# total size of allocations: %llu",
                    static_cast<unsigned long long>(stats.second));

  if (heap_.isCorrupted(begin)) {
    Printer::EmitError(OutFile,
                       "check", 
                       "heap is corrupted - leak checking is not possible");
    return;
  }

  if (Config.withLeaks) {
    emitLeaks(begin, regex);
  }

  Printer::EmitLine(OutFile, "");
}

////////////////////////////////////////////////////////////////////////////////
/// @brief print all leaks for the memory blocks, starting with the argument
////////////////////////////////////////////////////////////////////////////////

void Tracker::emitLeaks (MemoryAllocation const* heap, regex_t* regex) { 
  char memory[16384];

  int shown              = 0;
  uint64_t numLeaks      = 0;
  uint64_t numDuplicates = 0;
  uint64_t sizeLeaks     = 0;
  auto allocation = heap;

  std::unordered_set<uint64_t> seen;
  StackResolver resolver;

  while (allocation != nullptr) {
    char* stack = resolver.resolveStack(Config.maxFrames, 
                                        Printer::UseColors(OutFile), 
                                        &memory[0], 
                                        sizeof(memory), 
                                        allocation->stack);

    if (! mustSuppressLeak(stack, regex)) {

      if (stack != nullptr) {
        uint64_t hash = HashString(stack);

        if (seen.find(hash) != seen.end()) {
          // duplicate
          ++numDuplicates;
          sizeLeaks += allocation->size;

          allocation = allocation->next;
          continue;
        }

        seen.emplace(hash);
      }

      Printer::EmitError(OutFile,
                         "check", 
                         "leak of size %llu byte(s), allocated with via %s:",
                         static_cast<unsigned long long>(allocation->size),
                         MemoryAllocation::AccessTypeName(allocation->type));

      Printer::EmitLine(OutFile,
                        "%s", 
                        (stack ? stack : "  # no stack available"));
    
      ++numLeaks;
      sizeLeaks += allocation->size;

      if (++shown >= Config.maxLeaks) {
        Printer::EmitError(OutFile,   
                           "check",
                           "stopping output at %d unique leak(s), results are incomplete",
                           shown); 
        break;
      }
    }

    allocation = allocation->next;
  } 

  if (sizeLeaks == 0) {
    Printer::EmitLine(OutFile, "# no leaks found");
  }
  else {
    Printer::EmitError(OutFile,
                       "check", 
                       "found %llu unique leaks(s), %llu duplicates, with total size of %llu byte(s)",
                       static_cast<unsigned long long>(numLeaks),
                       static_cast<unsigned long long>(numDuplicates),
                       static_cast<unsigned long long>(sizeLeaks));
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                           public static variables
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief library malloc() function
////////////////////////////////////////////////////////////////////////////////

Tracker::MallocFuncType  Tracker::LibraryMalloc           = nullptr;

////////////////////////////////////////////////////////////////////////////////
/// @brief library calloc() function
////////////////////////////////////////////////////////////////////////////////

Tracker::CallocFuncType  Tracker::LibraryCalloc           = nullptr;

////////////////////////////////////////////////////////////////////////////////
/// @brief library realloc() function
////////////////////////////////////////////////////////////////////////////////

Tracker::ReallocFuncType Tracker::LibraryRealloc          = nullptr;

////////////////////////////////////////////////////////////////////////////////
/// @brief library free() function
////////////////////////////////////////////////////////////////////////////////

Tracker::FreeFuncType    Tracker::LibraryFree             = nullptr;

////////////////////////////////////////////////////////////////////////////////
/// @brief library exit() function
////////////////////////////////////////////////////////////////////////////////

Tracker::ExitFuncType    Tracker::LibraryExit             = nullptr;

////////////////////////////////////////////////////////////////////////////////
/// @brief library _exit() function
////////////////////////////////////////////////////////////////////////////////

Tracker::ExitFuncType    Tracker::Library_Exit            = nullptr;

////////////////////////////////////////////////////////////////////////////////
/// @brief tracker state
////////////////////////////////////////////////////////////////////////////////

Tracker::StateType       Tracker::State                   = STATE_UNINITIALIZED;

////////////////////////////////////////////////////////////////////////////////
/// @brief output stream
////////////////////////////////////////////////////////////////////////////////

FILE*                    Tracker::OutFile                 = stderr;

////////////////////////////////////////////////////////////////////////////////
/// @brief configuration
////////////////////////////////////////////////////////////////////////////////

Configuration            Tracker::Config;

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not finalization was run
////////////////////////////////////////////////////////////////////////////////

bool                     Tracker::Finalized               = false;

////////////////////////////////////////////////////////////////////////////////
/// @brief buffer for untracked pointers
////////////////////////////////////////////////////////////////////////////////

void*                    Tracker::UntrackedPointers[4096];

////////////////////////////////////////////////////////////////////////////////
/// @brief length of untracked pointers buffer
////////////////////////////////////////////////////////////////////////////////

size_t                   Tracker::UntrackedPointersLength = 0;

