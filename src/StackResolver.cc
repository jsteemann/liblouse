
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <sys/wait.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>

#include "StackResolver.h"
#include "Tracker.h"

using StackResolver = debugging::StackResolver;
using Tracker       = debugging::Tracker;

// -----------------------------------------------------------------------------
// --SECTION--                                          private helper functions
// -----------------------------------------------------------------------------

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

// -----------------------------------------------------------------------------
// --SECTION--                                               class StackResolver
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create the resolver
////////////////////////////////////////////////////////////////////////////////

StackResolver::StackResolver ()
  : directoryLength_(0) {

  determineProgname();
  determineDirectory();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief destroy the resolver
////////////////////////////////////////////////////////////////////////////////

StackResolver::~StackResolver () {
  for (auto& it : cache_) {
    Tracker::LibraryFree(it.second);
  }
  cache_.clear();
}

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief captures a stacktrace
////////////////////////////////////////////////////////////////////////////////

void** StackResolver::captureStackTrace (int maxFrames) {
  char memory[256];

  void** trace = reinterpret_cast<void**>(memory);

  int frames = maxFrames + 2;

  while (frames * sizeof(void*) > sizeof(memory)) {
    --frames;
  }

  int traceSize = 0;
  unw_cursor_t cursor; 
  unw_context_t uc;
  unw_word_t ip;

  ::unw_getcontext(&uc);
  ::unw_init_local(&cursor, &uc);

  while (traceSize < frames && ::unw_step(&cursor) > 0) {
    ::unw_get_reg(&cursor, UNW_REG_IP, &ip);
    trace[traceSize++] = (void*) ip;
  }

  if (traceSize < 2) {
    return nullptr;
  }

  void** pcs = static_cast<void**>(Tracker::LibraryMalloc(sizeof(void*) * traceSize));

  if (pcs == nullptr) {
    return nullptr;
  }

  ::memcpy(&pcs[0], trace + 1, sizeof(void*) * (traceSize - 1));
  pcs[traceSize - 1] = nullptr;

  return pcs;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief captures a stacktrace
////////////////////////////////////////////////////////////////////////////////

bool StackResolver::captureStackTrace (int maxFrames, void** memory, int length) {
  int frames = maxFrames + 2;

  while (frames >= length) {
    --frames;
  }

  int traceSize = 0;
  unw_cursor_t cursor; 
  unw_context_t uc;
  unw_word_t ip;

  ::unw_getcontext(&uc);
  ::unw_init_local(&cursor, &uc);

  int i = 0;
  while (traceSize < frames && ::unw_step(&cursor) > 0) {
    ::unw_get_reg(&cursor, UNW_REG_IP, &ip);
    if (i > 0) {
      memory[traceSize++] = (void*) ip;
    }
    ++i;
  }
  memory[i] = nullptr;

  if (traceSize < 1) {
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief converts a stacktrace into human-readable text
////////////////////////////////////////////////////////////////////////////////

char* StackResolver::resolveStack (int maxFrames, bool useColors, char* memory, size_t length, void** stack) {
  if (stack == nullptr) {
    return nullptr;
  }

  char* start = memory;
  int frames = 0;

  while (*stack != nullptr) {
    if (frames++ >= maxFrames) {
      break;
    }

    void* pc = *stack;

    auto it = cache_.find(pc);

    if (it != cache_.end()) {
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
        line = addr2line(useColors, 
                         progname(), 
                         pc, 
                         &memory);
      } 
      else {
        line = addr2line(useColors,
                         dlinf.dli_fname, 
                         reinterpret_cast<void*>(reinterpret_cast<char*>(pc) - reinterpret_cast<char*>(dlinf.dli_fbase)), 
                         &memory);
      }

      if (line == nullptr) {
        return nullptr;
      }

      size_t len = ::strlen(line); 
      char* copy = static_cast<char*>(Tracker::LibraryMalloc(len + 1));

      if (copy != nullptr) {
        ::memcpy(copy, line, len);
        copy[len] = '\0';

        try {
          cache_.emplace(pc, static_cast<char*>(copy));
        }
        catch (...) {
          Tracker::LibraryFree(copy);
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

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief calls addr2line 
////////////////////////////////////////////////////////////////////////////////

char* StackResolver::addr2line (bool useColors, char const* prog, void* pc, char** memory) {
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
    char const* env[] = { nullptr };

    if (::execle("/usr/bin/addr2line", "addr2line", PointerToAscii(pc, *memory), "-C", "-f", "-e", prog, nullptr, env) == -1) {
      ::close(pipefd[1]);
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
/*          
  if (::strstr(p, "debugging::Tracker::") != nullptr) {
    // exclude ourselves
    return *memory;
  }
*/
  if (::strstr(p, "__libc_start_main") != nullptr) {
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
    if (useColors) {
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

    if (directoryLength_ < l &&
        strncmp(nl + 1, directoryBuffer_, directoryLength_) == 0) {
      // strip pwd
      nl += directoryLength_;
      l -= directoryLength_;
    }

    ::memcpy(*memory, nl + 1, l);
    *memory += l;

    char const* b;
    if (useColors) {
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

void StackResolver::determineProgname () {
  ssize_t length = ::readlink("/proc/self/exe", &prognameBuffer_[0], sizeof(prognameBuffer_) - 1);

  if (length < 0) {
    length = 0;
  }

  prognameBuffer_[length] = '\0';
}

////////////////////////////////////////////////////////////////////////////////
/// @brief determines the current directory
////////////////////////////////////////////////////////////////////////////////

void StackResolver::determineDirectory () {
  size_t length = 0;

  if (::getcwd(&directoryBuffer_[0], sizeof(directoryBuffer_) - 2) != nullptr) {
    length = ::strlen(directoryBuffer_);
  }

  directoryBuffer_[length] = '/';
  directoryBuffer_[length + 1] = '\0';

  directoryLength_ = length + 1;
}

