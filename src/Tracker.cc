
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <cxxabi.h>
#include <sys/wait.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include "Tracker.h"
#include "MemoryAllocation.h"

using Configuration = debugging::Configuration;
using Tracker = debugging::Tracker;
using MemoryAllocation = debugging::MemoryAllocation;

// -----------------------------------------------------------------------------
// --SECTION--                                          private helper functions
// -----------------------------------------------------------------------------

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
/// @brief converts a uint64_t to a base16 string representation
////////////////////////////////////////////////////////////////////////////////
      
static char* NumberToAscii (uint64_t val, char* memory) {
  char* res = memory;

  if (val == 0) {
    res[0] = '0';
    res[1] = '\0';

    return res;
  }

  int const MaxLength = 32;
  res[MaxLength - 1] = '\0';

  int i;
  for (i = MaxLength - 2; val != 0 && i != 0; i--, val /= 16) {
    res[i] = "0123456789ABCDEF"[val % 16];
  }

  return &res[i + 1];
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a pointer to a hexadecimal string representation
////////////////////////////////////////////////////////////////////////////////

static char* PointerToAscii (void const* val, char* memory) {
  char* buf = NumberToAscii(reinterpret_cast<uint64_t>(val), memory + 32);

  // output format is 0x....
  ::memcpy(memory + 2, buf, ::strlen(buf)); 
  memory[0] = '0';
  memory[1] = 'x';

  return memory;
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
  : allocations(), directoryLength(0) {

  determineProgname();
  determineDirectory();

  Initialize();
  State = STATE_TRACING;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the tracker
/// this performs leak checking
////////////////////////////////////////////////////////////////////////////////

Tracker::~Tracker () {
  auto heap = allocations.begin();

  if (Config.suppressFilter != nullptr) {
    if (::regcomp(&leakRegex, Config.suppressFilter, REG_NOSUB | REG_EXTENDED) != 0) {
      Config.suppressFilter = nullptr;
    }
  }

  try {
    emitResults(heap);
  }
  catch (...) {
  }

  if (Config.suppressFilter != nullptr) {
    ::regfree(&leakRegex); 
  }
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

    LibraryMalloc  = malloc;
    LibraryCalloc  = calloc;
    LibraryRealloc = realloc;
    LibraryFree    = free;

    State = STATE_HOOKED;
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief print an error and abort execution
////////////////////////////////////////////////////////////////////////////////

void Tracker::ImmediateAbort (char const* type, char const* message) {
  EmitError(type, "%s", message);
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
    allocation->stack = captureStackTrace();
  }

  allocations.add(allocation);

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
    EmitError("runtime",
              "%s called with invalid memory pointer %p", 
              MemoryAllocation::AccessTypeName(type),
              pointer);

    emitStackTrace();
  }
  else {
    if (type != MemoryAllocation::MatchingFreeType(allocation->type)) {
      EmitError("runtime",
                "trying to %s memory pointer %p that was originally allocated via %s",
                MemoryAllocation::AccessTypeName(type),
                pointer,
                MemoryAllocation::AccessTypeName(allocation->type));

      emitStackTrace();

      if (allocation->stack != nullptr) {
        EmitLine("");
        EmitLine("original allocation site of memory pointer %p via %s:",
                 pointer,
                 MemoryAllocation::AccessTypeName(allocation->type));

        emitStackTrace(allocation->stack);
      }
    }

    if (! allocation->isTailSignatureValid()) {
      EmitError("runtime",
                "buffer overrun after memory pointer %p of size %llu that was originally allocated via %s",
                pointer,
                (unsigned long long) allocation->size,
                MemoryAllocation::AccessTypeName(allocation->type));

      emitStackTrace();

      if (allocation->stack != nullptr) {
        EmitLine("");
        EmitLine("original allocation site of memory pointer %p via %s:",
                 pointer,
                 MemoryAllocation::AccessTypeName(allocation->type));

        emitStackTrace(allocation->stack);
      }
    }
  }

  allocations.remove(allocation);

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

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not a leak should be suppressed
////////////////////////////////////////////////////////////////////////////////

bool Tracker::mustSuppressLeak (char const* text) {
  if (Config.suppressFilter == nullptr || 
      *Config.suppressFilter == '\0' ||
      text == nullptr) {
    return false;
  }

  if (::regexec(&leakRegex, text, 0, nullptr, 0) != 0) {
    return false;
  }

  return true;
}
    
////////////////////////////////////////////////////////////////////////////////
/// @brief prints the current stacktrace
////////////////////////////////////////////////////////////////////////////////

void Tracker::emitStackTrace () {
  void** stack = captureStackTrace();

  if (stack == nullptr) {
    return;
  }

  emitStackTrace(stack);

  LibraryFree(stack); 
}

////////////////////////////////////////////////////////////////////////////////
/// @brief prints the stacktrace for the stack argument
////////////////////////////////////////////////////////////////////////////////

void Tracker::emitStackTrace (void** stack) {
  if (stack == nullptr) {
    return;
  }

  std::unordered_map<void*, char*> cache;
  char memory[4096];

  char* buffer = resolveStack(cache, &memory[0], sizeof(memory), stack);

  if (buffer != nullptr) {
    EmitLine("%s", buffer);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief print overall results for the linked list of memory blocks,
/// starting with the argument
////////////////////////////////////////////////////////////////////////////////

void Tracker::emitResults (MemoryAllocation const* heap) {
  EmitLine("");
  EmitLine("RESULTS --------------------------------------------------------");
  EmitLine("");

  auto stats = allocations.totals();

  EmitLine("# total number of allocations: %llu",
           (unsigned long long) stats.first);

  EmitLine("# total size of allocations: %llu",
           (unsigned long long) stats.second);

  if (allocations.isCorrupted(heap)) {
    EmitError("check", 
              "heap is corrupted - leak checking is not possible");
    return;
  }

  if (Config.withLeaks) {
    emitLeaks(heap);
  }

  EmitLine("");
}

////////////////////////////////////////////////////////////////////////////////
/// @brief print all leaks for the memory blocks, starting with the argument
////////////////////////////////////////////////////////////////////////////////

void Tracker::emitLeaks (MemoryAllocation const* heap) { 
  std::unordered_map<void*, char*> cache;
  char memory[16384];

  int shown = 0;
  uint64_t numLeaks = 0;
  uint64_t sizeLeaks = 0;
  auto allocation = heap;

  while (allocation != nullptr) {
    char* stack = resolveStack(cache, &memory[0], sizeof(memory), allocation->stack);

    if (! mustSuppressLeak(stack)) {
      EmitError("check", 
                "leak of size %llu byte(s), allocated with via %s:",
                (unsigned long long) allocation->size,
                MemoryAllocation::AccessTypeName(allocation->type));

      EmitLine("%s", 
               (stack ? stack : "  # no stack available"));
    
      ++numLeaks;
      sizeLeaks += allocation->size;

      if (++shown >= Config.maxLeaks) {
        break;
      }
    }

    allocation = allocation->next;
  } 

  for (auto& it : cache) {
    LibraryFree(it.second);
  }

  if (sizeLeaks == 0) {
    EmitLine("# no leaks found");
  }
  else {
    EmitError("check", 
              "found %llu leaks(s) with total size of %llu byte(s)",
              (unsigned long long) numLeaks,
              (unsigned long long) sizeLeaks);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief emits a line
////////////////////////////////////////////////////////////////////////////////

void Tracker::EmitLine (char const* format, ...) { 
  char buffer[2048];

  va_list ap;
  va_start(ap, format);
  int length = ::vsnprintf(buffer, sizeof(buffer) - 1, format, ap);
  va_end(ap);

  buffer[sizeof(buffer) - 1] = '\0'; // paranoia

  if (length >= 0) {
    ::fprintf(OutFile, "%s\n", &buffer[0]);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief emits an error
////////////////////////////////////////////////////////////////////////////////

void Tracker::EmitError (char const* type, char const* format, ...) { 
  char buffer[2048];

  va_list ap;
  va_start(ap, format);
  int length = ::vsnprintf(buffer, sizeof(buffer) - 1, format, ap);
  va_end(ap);

  buffer[sizeof(buffer) - 1] = '\0'; // paranoia

  if (length > 0) {
    if (::isatty(::fileno(OutFile))) {
      ::fprintf(OutFile, "\n\033[31;1m%s error: %s\033[0m\n", type, &buffer[0]);
    }
    else {
      ::fprintf(OutFile, "\n%s error: %s\n", type, &buffer[0]);
    }
    ::fflush(OutFile);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief captures a stacktrace
////////////////////////////////////////////////////////////////////////////////

void** Tracker::captureStackTrace () {
  char memory[256];

  void** trace = reinterpret_cast<void**>(memory);

  int frames = Config.maxFrames + 2;

  while (frames * sizeof(void*) > sizeof(memory)) {
    --frames;
  }

  int traceSize = 0;
  unw_cursor_t cursor; 
  unw_context_t uc;
  unw_word_t ip;

  unw_getcontext(&uc);
  unw_init_local(&cursor, &uc);

  while (traceSize < frames && unw_step(&cursor) > 0) {
    unw_get_reg(&cursor, UNW_REG_IP, &ip);
    trace[traceSize++] = (void*) ip;
  }

  if (traceSize < 2) {
    return nullptr;
  }

  void** pcs = static_cast<void**>(LibraryMalloc(sizeof(void*) * traceSize));

  if (pcs == nullptr) {
    return nullptr;
  }

  ::memcpy(&pcs[0], trace + 1, sizeof(void*) * (traceSize - 1));
  pcs[traceSize - 1] = nullptr;

  return pcs;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a stacktrace into human-readable text
////////////////////////////////////////////////////////////////////////////////

char* Tracker::resolveStack (std::unordered_map<void*, char*>& cache, char* memory, size_t length, void** stack) {
  if (stack == nullptr) {
    return nullptr;
  }

  char* start = memory;
  int frames = 0;

  while (*stack != nullptr) {
    if (frames++ >= Config.maxFrames) {
      break;
    }

    void* pc = *stack;

    auto it = cache.find(pc);

    if (it != cache.end()) {
      size_t len = ::strlen((*it).second);
      ::memcpy(memory, (*it).second, len);
      memory += len;
    } 
    else {
      Dl_info dlinf;
      char* line;
      if (::dladdr(pc, &dlinf) == 0 || 
          dlinf.dli_fname[0] != '/' || 
          ! ::strcmp(progname(), dlinf.dli_fname)) {
        line = addr2line(progname(), pc, &memory);
      } 
      else {
        line = addr2line(dlinf.dli_fname, reinterpret_cast<void*>(reinterpret_cast<char*>(pc) - reinterpret_cast<char*>(dlinf.dli_fbase)), &memory);
      }

      if (line == nullptr) {
        return nullptr;
      }

      size_t len = ::strlen(line); 
      char* copy = static_cast<char*>(LibraryMalloc(len + 1));

      if (copy != nullptr) {
        ::memcpy(copy, line, len);
        copy[len] = '\0';

        try {
          cache.emplace(pc, static_cast<char*>(copy));
        }
        catch (...) {
          LibraryFree(copy);
        }
      }
    }

    *memory = '\0';

    if (static_cast<size_t>(memory - start) + 1024 >= length) {
      // we're about to run out of memory
      break;
    }

    ++stack;
  } 

  *memory = '\0';

  if (frames > 0 && memory > start) {
    --memory;
    *memory = '\0';
  }

  return start;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief calls addr2line 
////////////////////////////////////////////////////////////////////////////////

char* Tracker::addr2line (char const* prog, void* pc, char** memory) {
  int pipefd[2];

  if (::pipe(pipefd) != 0) {
    return nullptr;
  }

  pid_t pid = ::fork();

  if (pid == 0) {
    ::close(pipefd[0]);
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);

    // do not pass LD_PRELOAD to sub-shell    
    char const* env[] = { "LD_PRELOAD=", nullptr };

    if (::execle("/usr/bin/addr2line", "addr2line", PointerToAscii(pc, *memory), "-C", "-f", "-e", prog, nullptr, env) == -1) {
      ::close(pipefd[1]);
      EmitError("internal",
                "unable to invoke /usr/bin/addr2line");
      std::exit(1);
    }
  }

  ::close(pipefd[1]);

  char lineBuffer[1024];
  ssize_t len = ::read(pipefd[0], &lineBuffer[0], sizeof(lineBuffer) - 1);
  ::close(pipefd[0]);

  if (len == 0) {
    return nullptr;
  }

  lineBuffer[len] = '\0';

  if (::waitpid(pid, nullptr, 0) != pid) {
    return nullptr;
  }

  char* p = &lineBuffer[0];
  char* nl = ::strchr(p, '\n');
          
  if (::strstr(p, "debugging::Tracker::") != nullptr) {
    return *memory;
  }
  
  char* old = *memory;
  char const* a = "  # ";
  ::memcpy(*memory, a, ::strlen(a));
  *memory += ::strlen(a);

  if (nl != nullptr) {
    // function name
    ::memcpy(*memory, p, nl - p);
    *memory += nl - p;

    char const* a;
    if (::isatty(::fileno(OutFile))) {
      a = " (\033[33m";
    }
    else {
      a = " (";
    }
    ::memcpy(*memory, a, ::strlen(a));
    *memory += ::strlen(a);

    // filename
    size_t l = ::strlen(nl + 1);
    if (l > 1 && nl[l] == '\n') {
      --l;
    }

    if (directoryLength < l &&
        strncmp(nl + 1, directoryBuffer, directoryLength) == 0) {
      // strip pwd
      nl += directoryLength;
      l -= directoryLength;
    }

    ::memcpy(*memory, nl + 1, l);
    *memory += l;

    char const* b;
    if (::isatty(::fileno(OutFile))) {
      b = "\033[0m)\n";
    } 
    else {
      b = ")\n";
    }
    ::memcpy(*memory, b, ::strlen(b));
    *memory += ::strlen(b);
  } 
  else {
    ::memcpy(*memory, p, len);
    *memory += len;
  }

  **memory = '\0';

  return old;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determines the name of the executable
////////////////////////////////////////////////////////////////////////////////

void Tracker::determineProgname () {
  ssize_t length = ::readlink("/proc/self/exe", &prognameBuffer[0], sizeof(prognameBuffer) - 1);

  if (length < 0) {
    length = 0;
  }
  prognameBuffer[length] = '\0';
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determines the current directory
////////////////////////////////////////////////////////////////////////////////

void Tracker::determineDirectory () {
  size_t length = 0;

  if (::getcwd(&directoryBuffer[0], sizeof(directoryBuffer) - 2) != nullptr) {
    length = ::strlen(directoryBuffer);
  }
  directoryBuffer[length] = '/';
  directoryBuffer[length + 1] = '\0';

  directoryLength = length + 1;
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
/// @brief buffer for untracked pointers
////////////////////////////////////////////////////////////////////////////////

void*                    Tracker::UntrackedPointers[4096];

////////////////////////////////////////////////////////////////////////////////
/// @brief length of untracked pointers buffer
////////////////////////////////////////////////////////////////////////////////

size_t                   Tracker::UntrackedPointersLength = 0;


