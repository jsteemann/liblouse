
#include <cstdlib>
#include <stdio.h>
#include <dlfcn.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdarg.h>
#include <unordered_map>
#include <new>
#include <limits>
#include <functional>
#include <execinfo.h>
#include <cxxabi.h>
#include <cstring>
#include <mutex>
#include <regex.h>

#define UNW_LOCAL_ONLY
#include <libunwind.h>

static void* TemporaryMalloc (size_t) {
  return nullptr;
}

static void* TemporaryCalloc (size_t, size_t) {
  return nullptr;
}

static void* TemporaryRealloc (void*, size_t) {
  return nullptr;
}

static bool BooleanArgument (char const* value, bool defaultValue) {
  if (::strcmp(value, "on") ==  0 || 
      ::strcmp(value, "1") == 0 || 
      ::strcmp(value, "true") == 0 || 
      ::strcmp(value, "yes") == 0) {
    return true;
  }

  if (::strcmp(value, "off") ==  0 || 
      ::strcmp(value, "0") == 0 || 
      ::strcmp(value, "false") == 0 || 
      ::strcmp(value, "no") == 0) {
    return false;
  }

  return defaultValue;
}

namespace debugging {
  constexpr size_t roundup (size_t value) {
    return (value + 15) - ((value + 15) & 15);
  }

  struct MemoryAllocation {
    enum AccessType : uint32_t {
      TYPE_INVALID = 0,
      TYPE_NEW,
      TYPE_NEW_ARRAY,
      TYPE_MALLOC,
      TYPE_DELETE,
      TYPE_DELETE_ARRAY,
      TYPE_FREE
    };

    MemoryAllocation () = delete;

    ~MemoryAllocation () = delete;

    void init (size_t size, AccessType type) {
      this->size         = size;
      this->stack        = nullptr;
      this->ownSignature = MemoryAllocation::OwnSignature;
      this->type         = type;
      this->prev         = nullptr;
      this->next         = nullptr;

      ::memcpy(tailSignatureAddress(), &TailSignature, sizeof(TailSignature));
    }

    void wipeSignature () {
      ownSignature = WipedSignature;     
    }

    void* memory () {
      return static_cast<void*>(reinterpret_cast<char*>(this) + OwnSize());
    }

    void const* memory () const {
      return static_cast<void const*>(reinterpret_cast<char const*>(this) + OwnSize());
    }

    void* tailSignatureAddress () {
      return static_cast<char*>(memory()) + size;
    }

    void const* tailSignatureAddress () const {
      return static_cast<char const*>(memory()) + size;
    }

    bool isValid () const {
      return (isOwnSignatureValid() && isTailSignatureValid());
    }

    bool isOwnSignatureValid () const {
      return ownSignature == OwnSignature;
    }

    bool isTailSignatureValid () const {
      return (::memcmp(tailSignatureAddress(), &TailSignature, sizeof(TailSignature)) == 0);
    }

    static size_t OwnSize () {
      return roundup(sizeof(MemoryAllocation));
    }

    static size_t TotalSize () {
      return OwnSize() + sizeof(TailSignature);
    }

    static char const* AccessTypeName (AccessType type) {
      switch (type) {
        case TYPE_NEW:          return "new";
        case TYPE_NEW_ARRAY:    return "new[]";
        case TYPE_MALLOC:       return "malloc()";
        case TYPE_DELETE:       return "delete";
        case TYPE_DELETE_ARRAY: return "delete[]";
        case TYPE_FREE:         return "free()";
        default:                return "invalid"; 
      }
    }

    static AccessType MatchingFreeType (AccessType type) {
      switch (type) {
        case TYPE_NEW:          return TYPE_DELETE;
        case TYPE_NEW_ARRAY:    return TYPE_DELETE_ARRAY;
        case TYPE_MALLOC:       return TYPE_FREE;
        default:                return TYPE_INVALID;
      }
    }

    size_t            size;
    void**            stack;
    AccessType        type;
    uint32_t          ownSignature;
    MemoryAllocation* prev;
    MemoryAllocation* next;

    static const uint32_t OwnSignature;
    static const uint32_t TailSignature;
    static const uint32_t WipedSignature;
  };

  class MemoryAllocations {
    
    public:

      MemoryAllocations ()
        : lock(), head(nullptr), numAllocations(0), sizeAllocations(0) {
      }

      void add (MemoryAllocation* allocation) {
        std::lock_guard<std::mutex> locker(lock);

        allocation->prev = nullptr;
        allocation->next = head;
        if (head != nullptr) {
          head->prev = allocation;
        }
        head = allocation;

        ++numAllocations;
        sizeAllocations += allocation->size;
      }

      void remove (MemoryAllocation* allocation) {
        std::lock_guard<std::mutex> locker(lock);

        if (allocation->prev != nullptr) {
          allocation->prev->next = allocation->next;
        }

        if (allocation->next != nullptr) {
          allocation->next->prev = allocation->prev;
        }

        if (head == allocation) {
          head = allocation->next;
        }
      }
  
      MemoryAllocation* begin () const {
        return head;
      }

      std::pair<uint64_t, uint64_t> totals () const {
        return std::make_pair(numAllocations, sizeAllocations); 
      }

      bool isHeapCorrupted (MemoryAllocation const* heap) const {
        auto allocation = heap;

        while (allocation != nullptr) {
          if (! allocation->isOwnSignatureValid()) {
            return true;
          }

          allocation = allocation->next;
        }

        return false;
      }
      
    private:

      std::mutex lock;
      MemoryAllocation* head;
      uint64_t numAllocations;
      uint64_t sizeAllocations;

  };


  class MemoryDebugger {

    public:

      enum StateType {
        STATE_UNINITIALIZED,
        STATE_INITIALIZING,
        STATE_HOOKED,
        STATE_TRACING
      };

      MemoryDebugger ()
        : allocations(), directoryLength(0) {

        determineProgname();
        determineDirectory();

        Initialize();
        State = STATE_TRACING;
      }

      ~MemoryDebugger () {
        auto heap = allocations.begin();

        if (LeakFilter != nullptr) {
          if (regcomp(&leakRegex, LeakFilter, REG_NOSUB | REG_EXTENDED) != 0) {
            LeakFilter = nullptr;
          }
        }

        try {
          printResults(heap);
        }
        catch (...) {
        }

        if (LeakFilter != nullptr) {
          regfree(&leakRegex); 
        }
      }

      static void ImmediateAbort (char const* type, char const* message) {
        EmitError(type, "%s", message);
        std::abort();
      }

      static void Initialize () {
        if (State == STATE_UNINITIALIZED) {
          State = STATE_INITIALIZING;

          OriginalMalloc  = TemporaryMalloc;
          OriginalCalloc  = TemporaryCalloc;
          OriginalRealloc = TemporaryRealloc;

          auto malloc = getFunction<MallocFuncType>("malloc");

          if (malloc == nullptr) {
            ImmediateAbort("init", "cannot find malloc()");
          }

          auto calloc = getFunction<CallocFuncType>("calloc");

          if (calloc == nullptr) {
            ImmediateAbort("init", "cannot find calloc()");
          }

          auto realloc = getFunction<ReallocFuncType>("realloc");

          if (realloc == nullptr) {
            ImmediateAbort("init", "cannot find realloc()");
          }

          auto free = getFunction<FreeFuncType>("free");

          if (free == nullptr) {
            ImmediateAbort("init", "cannot find free()");
          }

          OriginalMalloc  = malloc;
          OriginalCalloc  = calloc;
          OriginalRealloc = realloc;
          OriginalFree    = free;

          State = STATE_HOOKED;
  
        }
      }

      static void* AllocateInitialMemory (size_t size) { 
        if (InitialPointersLength == sizeof(InitialPointers) / sizeof(InitialPointers[0])) {
          ImmediateAbort("allocation", "malloc: out of initialization memory\n");
        }

        void* pointer = OriginalMalloc(size + sizeof(size_t));

        if (pointer != nullptr) {
          size_t* memory = static_cast<size_t*>(pointer);
          *memory = size;
          ++memory;

          pointer = static_cast<void*>(memory);
          InitialPointers[InitialPointersLength++] = pointer;
        }

        return pointer;
      }

      static bool FreeInitialMemory (void* pointer) {
        for (size_t i = 0; i < InitialPointersLength; ++i) {
          if (InitialPointers[i] == pointer) {
            size_t* memory = static_cast<size_t*>(pointer);
            --memory;
            debugging::MemoryDebugger::OriginalFree(static_cast<void*>(memory));
            --InitialPointersLength;

            for (size_t j = i; j < InitialPointersLength; ++j) {
              InitialPointers[j] = InitialPointers[j + 1];
            }
            return true;
          }
        }

        return false;
      }

      size_t memorySize (void* pointer) const {
        for (size_t i = 0; i < InitialPointersLength; ++i) {
          if (InitialPointers[i] == pointer) {
            size_t* memory = static_cast<size_t*>(pointer);
            --memory;
            return *memory;
          }
        }

        void* mem = static_cast<void*>(static_cast<char*>(pointer) - MemoryAllocation::OwnSize());
        auto allocation = static_cast<MemoryAllocation*>(mem);

        if (allocation->isOwnSignatureValid()) {
          return allocation->size;
        }

        // unknown memory
        return 0;
      }

      void* allocateMemory (size_t size, MemoryAllocation::AccessType type) {
        // ::fprintf(stderr, "allocate memory called, size: %lu\n", (unsigned long) size);
        size_t const actualSize = size + MemoryAllocation::TotalSize();
        void* pointer = OriginalMalloc(actualSize);

        if (pointer == nullptr || State != STATE_TRACING) {
          // ::fprintf(stderr, "allocate returning pointer %p\n", pointer);
          return pointer;
        }

        auto allocation = static_cast<MemoryAllocation*>(pointer);

        allocation->init(size, type);

        if (WithTraces) { 
          allocation->stack = captureStackTrace();
        }

        allocations.add(allocation);

        // ::fprintf(stderr, "allocate returning wrapped pointer %p, orig: %p\n", allocation->memory(), pointer);
        return allocation->memory();
      }

      void freeMemory (void* pointer, MemoryAllocation::AccessType type) {
        // ::fprintf(stderr, "freeMemory called with %p\n", pointer);
        if (pointer == nullptr) {
          return; 
        }

        if (InitialPointersLength > 0) {
          if (FreeInitialMemory(pointer)) {
            return;
          }
        }

        if (State != STATE_TRACING) {
          // ::fprintf(stderr, "call free with %p\n", pointer);
          OriginalFree(pointer);
          return;
        }

        void* mem = static_cast<void*>(static_cast<char*>(pointer) - MemoryAllocation::OwnSize());

        auto allocation = static_cast<MemoryAllocation*>(mem);

        if (! allocation->isOwnSignatureValid()) {
          EmitError("runtime",
                    "%s called with invalid memory pointer %p", 
                    MemoryAllocation::AccessTypeName(type),
                    pointer);

          printStackTrace();
        }
        else {
          if (type != MemoryAllocation::MatchingFreeType(allocation->type)) {
            EmitError("runtime",
                      "trying to %s memory pointer %p that was originally allocated via %s",
                      MemoryAllocation::AccessTypeName(type),
                      pointer,
                      MemoryAllocation::AccessTypeName(allocation->type));

            printStackTrace();

            if (allocation->stack != nullptr) {
              EmitLine("");
              EmitLine("original allocation site of memory pointer %p via %s:",
                       pointer,
                       MemoryAllocation::AccessTypeName(allocation->type));

              printStackTrace(allocation->stack);
            }
          }

          if (! allocation->isTailSignatureValid()) {
            EmitError("runtime",
                      "buffer overrun after memory pointer %p of size %llu that was originally allocated via %s",
                      pointer,
                      (unsigned long long) allocation->size,
                      MemoryAllocation::AccessTypeName(allocation->type));

            printStackTrace();

            if (allocation->stack != nullptr) {
              EmitLine("");
              EmitLine("original allocation site of memory pointer %p via %s:",
                       pointer,
                       MemoryAllocation::AccessTypeName(allocation->type));

              printStackTrace(allocation->stack);
            }
          }
        }

        allocations.remove(allocation);
     
        allocation->wipeSignature();
 
        if (allocation->stack != nullptr) {
          OriginalFree(allocation->stack);
        }
        OriginalFree(mem);
      } 

    private:

      void printStackTrace () {
        void** stack = captureStackTrace();

        if (stack == nullptr) {
          return;
        }
 
        printStackTrace(stack);

        OriginalFree(stack); 
      }

      void printStackTrace (void** stack) {
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

      void printResults (MemoryAllocation const* heap) {
        EmitLine("");
        EmitLine("LOUSE DONE --------------------------------------------------------");
        EmitLine("");

        auto stats = allocations.totals();

        EmitLine("# total number of allocations: %llu",
                 (unsigned long long) stats.first);

        EmitLine("# total size of allocations: %llu",
                 (unsigned long long) stats.second);

        if (allocations.isHeapCorrupted(heap)) {
          EmitError("check", 
                    "heap is corrupted - leak checking is not possible");
          return;
        }

        if (WithLeaks) {
          printLeaks(heap);
        }

        EmitLine("");
      }

      bool suppressLeak (char const* text) {
        if (LeakFilter == nullptr || 
            *LeakFilter == '\0' ||
            text == nullptr) {
          return false;
        }

        if (regexec(&leakRegex, text, 0, nullptr, 0) != 0) {
          return false;
        }

        return true;
      }
    
      void printLeaks (MemoryAllocation const* heap) { 
        std::unordered_map<void*, char*> cache;
        char memory[16384];

        uint64_t numLeaks = 0;
        uint64_t sizeLeaks = 0;
        auto allocation = heap;

        while (allocation != nullptr) {
          char* stack = resolveStack(cache, &memory[0], sizeof(memory), allocation->stack);

          if (! suppressLeak(stack)) {
            EmitError("check", 
                      "leak of size %llu byte(s), allocated with via %s:",
                      (unsigned long long) allocation->size,
                      MemoryAllocation::AccessTypeName(allocation->type));

            EmitLine("%s", 
                     (stack ? stack : "  # no stack available"));
          
            ++numLeaks;
            sizeLeaks += allocation->size;
          }

          allocation = allocation->next;
        } 

        for (auto& it : cache) {
          OriginalFree(it.second);
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

      static void EmitLine (char const* format, ...) { 
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

      static void EmitError (char const* type, char const* format, ...) { 
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

      template<typename T> static T getFunction (char const* name) {
        return reinterpret_cast<T>(::dlsym(RTLD_NEXT, name));
      }

      char const* progname () const {
        return &prognameBuffer[0];
      }

      void** captureStackTrace () {
        char memory[256];

        void** trace = reinterpret_cast<void**>(memory);

        int frames = MaxFrames + 2;

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
  
        void** pcs = static_cast<void**>(OriginalMalloc(sizeof(void*) * traceSize));

        if (pcs == nullptr) {
          return nullptr;
        }

        ::memcpy(&pcs[0], trace + 1, sizeof(void*) * (traceSize - 1));
        pcs[traceSize - 1] = nullptr;

        return pcs;
      }

      char* resolveStack (std::unordered_map<void*, char*>& cache, char* memory, size_t length, void** stack) {
        if (stack == nullptr) {
          return nullptr;
        }

        char* start = memory;
        int frames = 0;

        while (*stack != nullptr) {
          if (frames++ >= MaxFrames) {
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
            char* copy = static_cast<char*>(OriginalMalloc(len + 1));

            if (copy != nullptr) {
              ::memcpy(copy, line, len);
              copy[len] = '\0';

              try {
                cache.emplace(pc, static_cast<char*>(copy));
              }
              catch (...) {
                OriginalFree(copy);
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

      char* addr2line (char const* prog, void* pc, char** memory) {
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

          if (::execle("/usr/bin/addr2line", "addr2line", ptoa(pc, *memory), "-C", "-f", "-e", prog, nullptr, env) == -1) {
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
        char* nl = strchr(p, '\n');
                
        if (strstr(p, "debugging::MemoryDebugger::") != nullptr) {
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

      char* utoa (uint64_t val, char* memory, int base = 10) {
        char* res = memory;

        if (val == 0) {
          res[0] = '0';
          res[1] = '\0';

          return res;
        }

        int const MaxLength = 32;
        res[MaxLength - 1] = '\0';

        int i;
        for (i = MaxLength - 2; val != 0 && i != 0; i--, val /= base) {
          res[i] = "0123456789ABCDEF"[val % base];
        }

        return &res[i + 1];
      }

      char* ptoa (void const* val, char* memory) {
        char* buf = utoa(reinterpret_cast<uint64_t>(val), memory + 32, 16);
        char* result = memory;  
        ::strcpy(result + 2, buf); 
        result[0] = '0';
        result[1] = 'x';
    
        return result;
      }

      void determineProgname () {
        ssize_t length = ::readlink("/proc/self/exe", &prognameBuffer[0], sizeof(prognameBuffer) - 1);

        if (length < 0) {
          length = 0;
        }
        prognameBuffer[length] = '\0';
      }

      void determineDirectory () {
        size_t length = 0;

        if (::getcwd(&directoryBuffer[0], sizeof(directoryBuffer) - 2) != nullptr) {
          length = ::strlen(directoryBuffer);
        }
        directoryBuffer[length] = '/';
        directoryBuffer[length + 1] = '\0';

        directoryLength = length + 1;
      }

    public:

      typedef void* (*MallocFuncType) (size_t);
      typedef void* (*CallocFuncType) (size_t, size_t);
      typedef void* (*ReallocFuncType) (void*, size_t);
      typedef void (*FreeFuncType) (void*);

      // the wrapped functions
      static MallocFuncType OriginalMalloc;
      static CallocFuncType OriginalCalloc;
      static ReallocFuncType OriginalRealloc;
      static FreeFuncType OriginalFree;
      static StateType State;
      static char const* LeakFilter;
      static FILE* OutFile;
      static bool WithLeaks;
      static bool WithTraces;
      static int MaxFrames;

    private:

      MemoryAllocations allocations;

      char prognameBuffer[512];
      char directoryBuffer[512];
      size_t directoryLength;
      regex_t leakRegex;

      static void* InitialPointers[4096];
      static size_t InitialPointersLength;
  };
}

uint32_t const debugging::MemoryAllocation::OwnSignature   = 0xdeadcafe;
uint32_t const debugging::MemoryAllocation::TailSignature  = 0xdeadbeef;
uint32_t const debugging::MemoryAllocation::WipedSignature = 0xb00bc0de;

debugging::MemoryDebugger::MallocFuncType  debugging::MemoryDebugger::OriginalMalloc   = nullptr;
debugging::MemoryDebugger::CallocFuncType  debugging::MemoryDebugger::OriginalCalloc   = nullptr;
debugging::MemoryDebugger::ReallocFuncType debugging::MemoryDebugger::OriginalRealloc  = nullptr;
debugging::MemoryDebugger::FreeFuncType    debugging::MemoryDebugger::OriginalFree     = nullptr;
debugging::MemoryDebugger::StateType       debugging::MemoryDebugger::State            = STATE_UNINITIALIZED;
char const*                                debugging::MemoryDebugger::LeakFilter       = "";
FILE*                                      debugging::MemoryDebugger::OutFile          = stderr;
bool                                       debugging::MemoryDebugger::WithLeaks        = true;
bool                                       debugging::MemoryDebugger::WithTraces       = true;
int                                        debugging::MemoryDebugger::MaxFrames        = 16;

void* debugging::MemoryDebugger::InitialPointers[4096];
size_t debugging::MemoryDebugger::InitialPointersLength = 0;

static debugging::MemoryDebugger Debugger;


__attribute__((constructor)) void InitLibrary () { 
  char const* value;

  value = ::getenv("LOUSE_WITHLEAKS");

  if (value != nullptr) {
    debugging::MemoryDebugger::WithLeaks = BooleanArgument(value, debugging::MemoryDebugger::WithLeaks);
  }

  value = ::getenv("LOUSE_WITHTRACES");

  if (value != nullptr) {
    debugging::MemoryDebugger::WithTraces = BooleanArgument(value, debugging::MemoryDebugger::WithTraces);
  }

  value = ::getenv("LOUSE_FILTER");

  if (value != nullptr) {
    debugging::MemoryDebugger::LeakFilter = value;
  }

  value = ::getenv("LOUSE_FRAMES");

  if (value != nullptr) {
    try {
      debugging::MemoryDebugger::MaxFrames = std::stol(value);
      if (debugging::MemoryDebugger::MaxFrames < 1) {
        debugging::MemoryDebugger::MaxFrames = 1;
      }
    }
    catch (...) {
    }
  }
}

 
// new
void* operator new (size_t size) throw(std::bad_alloc) {
  void* pointer = Debugger.allocateMemory(size, debugging::MemoryAllocation::TYPE_NEW);

  if (pointer == nullptr) {
    throw std::bad_alloc();
  }

  return pointer;
}

void* operator new (size_t size, std::nothrow_t const&) throw() {
  return Debugger.allocateMemory(size, debugging::MemoryAllocation::TYPE_NEW);
}

void* operator new[] (size_t size) throw(std::bad_alloc) {
  void* pointer = Debugger.allocateMemory(size, debugging::MemoryAllocation::TYPE_NEW_ARRAY);

  if (pointer == nullptr) {
    throw std::bad_alloc();
  }

  return pointer;
}

void* operator new[] (size_t size, std::nothrow_t const&) throw() {
  return Debugger.allocateMemory(size, debugging::MemoryAllocation::TYPE_NEW_ARRAY);
}

// delete
void operator delete (void* pointer) throw() {
  Debugger.freeMemory(pointer, debugging::MemoryAllocation::TYPE_DELETE);
}

void operator delete (void* pointer, std::nothrow_t const&) throw() {
  Debugger.freeMemory(pointer, debugging::MemoryAllocation::TYPE_DELETE);
}

void operator delete[] (void* pointer) throw() {
  Debugger.freeMemory(pointer, debugging::MemoryAllocation::TYPE_DELETE_ARRAY);
}

void operator delete[] (void* pointer, std::nothrow_t const&) throw() {
  Debugger.freeMemory(pointer, debugging::MemoryAllocation::TYPE_DELETE_ARRAY);
}

// malloc
void* malloc (size_t size) {
  // ::fprintf(stderr, "malloc size %lu\n", (unsigned long) size);

  // we don't treat malloc(0) specially here

  if (debugging::MemoryDebugger::State == debugging::MemoryDebugger::STATE_UNINITIALIZED) {
    debugging::MemoryDebugger::Initialize();
  }
 
  void* pointer;
  if (debugging::MemoryDebugger::State == debugging::MemoryDebugger::STATE_TRACING) {
    pointer = Debugger.allocateMemory(size, debugging::MemoryAllocation::TYPE_MALLOC);
  }
  else {
    pointer = debugging::MemoryDebugger::AllocateInitialMemory(size);
  }

  if (pointer == nullptr) {
    errno = ENOMEM;
  }

  return pointer;
}

// calloc
void* calloc (size_t nmemb, size_t size) {
  // ::fprintf(stderr, "calloc nmemb %lu size %lu\n", (unsigned long) nmemb, (unsigned long) size);

  // we don't treat calloc(0, x) or calloc(x, 0) specially here

  size_t const totalSize = nmemb * size;

  if (debugging::MemoryDebugger::State == debugging::MemoryDebugger::STATE_UNINITIALIZED) {
    debugging::MemoryDebugger::Initialize();
  }
  
  void* pointer;
  if (debugging::MemoryDebugger::State == debugging::MemoryDebugger::STATE_TRACING) {
    pointer = Debugger.allocateMemory(totalSize, debugging::MemoryAllocation::TYPE_MALLOC);
  }
  else {
    pointer = debugging::MemoryDebugger::AllocateInitialMemory(totalSize);
  }

  if (pointer == nullptr) {
    errno = ENOMEM;
  }
  else {
    memset(pointer, 0, totalSize);
  }

  return pointer;
}

// realloc
void* realloc (void* pointer, size_t size) {
  // ::fprintf(stderr, "realloc, pointer %p size %lu\n", pointer, (unsigned long) size);

  if (debugging::MemoryDebugger::State == debugging::MemoryDebugger::STATE_UNINITIALIZED) {
    debugging::MemoryDebugger::Initialize();
  }

  if (pointer == nullptr) {
    // same as malloc(size)
    void* memory;
    if (debugging::MemoryDebugger::State == debugging::MemoryDebugger::STATE_TRACING) {
      memory = Debugger.allocateMemory(size, debugging::MemoryAllocation::TYPE_MALLOC);
    }
    else {
      memory = debugging::MemoryDebugger::AllocateInitialMemory(size);
    }

    if (memory == nullptr) {
      errno = ENOMEM;
    }

    return memory;
  }
    
  size_t oldSize = Debugger.memorySize(pointer);

  if (oldSize >= size) {
    return pointer;
  }

  void* memory = Debugger.allocateMemory(size, debugging::MemoryAllocation::TYPE_MALLOC);

  if (memory == nullptr) {
    errno = ENOMEM;
  }
  else {
    ::memcpy(memory, pointer, oldSize);
    Debugger.freeMemory(pointer, debugging::MemoryAllocation::TYPE_FREE);
  }

  return memory;
}

// free
void free (void* pointer) {
  // ::fprintf(stderr, "free(%p)\n", pointer);

  if (pointer == nullptr) {
    return;
  }

  if (debugging::MemoryDebugger::State == debugging::MemoryDebugger::STATE_UNINITIALIZED) {
    debugging::MemoryDebugger::Initialize();
  }

  if (debugging::MemoryDebugger::State == debugging::MemoryDebugger::STATE_TRACING) {
    Debugger.freeMemory(pointer, debugging::MemoryAllocation::TYPE_FREE);
  }
  else{
    debugging::MemoryDebugger::FreeInitialMemory(pointer);
  }
}

// non-implemented functions
int posix_memalign (void** /*memptr*/, size_t /*alignment*/, size_t /*size*/) {
  debugging::MemoryDebugger::ImmediateAbort("assertion", "posix_memalign() is not handled");
  return 0;
}

void* aligned_alloc (size_t /*alignment*/, size_t /*size*/) {
  debugging::MemoryDebugger::ImmediateAbort("assertion", "aligned_alloc() is not handled");
  return nullptr;
}

