louse - a lousy memory issue tracker
====================================

Purpose
-------

louse can be used to find memory allocation and deallocation
errors as well as memory leaks in Linux executables.

It uses the LD_PRELOAD trick, so in general it could be used with 
existing executables and does not require recompiling or relinking
them. However, it will only produce meaningful output (stacktraces)
if the executable is compiled with debug symbols. For reporting
memory leaks, louse also requires the executable to terminate
regulary and call the regular exit handlers on shutdown.

Due its very limited scope, louse is often much faster than Valgrind.

louse can be used in the development cycle in favor of Valgrind for
quickly checking

* whether an executable uses `malloc`, `calloc`, `realloc`, `free`,
  `new`, `new[]`, `delete`, and `delete[]` correctly - or messes up
  their usage

* whether or not an executable leaks memory 

Executable startup times and turnaround times can be much lower with 
louse, making it an alternative in situations that demand quick
feedback. louse should be considered an additional tool that extends
the programmer's toolbox.

louse is by no means a replacement for Valgrind, not even for its 
memcheck tool. Valgrind and memcheck can do so much more than louse 
will ever by capable of. They are also more versatile, more robust,
more portable, more supported etc.

I still highly recommend using Valgrind for testing. 


How it works
------------

louse keeps track of all memory allocations by intercepting
all calls to `malloc`, `calloc` and `realloc`. All calls to these
operations are recorded by louse.

When memory is freed via `free`, louse will check if the memory 
location passed to `free` is actually valid by looking into its
internal list of allocations. Additionally it will perform a simple 
buffer overrun check on `free` to test if the executable wrote 
over the edge of the allocated memory region. If so, louse will
print an error and a stacktrace so the programmer can easily
find the source of the problem.

louse also wraps the C++ constructs `new`, `new[]`, `delete` 
and `delete[]`. When memory is deallocated, it will check if the
correct deallocation method is used (i.e. memory that was allocated
via `malloc`, `calloc` or `realloc` must be deallocated via `free`,
memory that was allocated via `new` must be deallocated via delete, and
memory that was allocated via `new[]` must be deallocated via `delete[]`).
If a wrong deallocation method is used, louse will print an error
and a stack trace.

By default louse will also check at program termination if there
are any memory leaks. For each memory leak, the stacktrace of the
allocation is shown.


Prerequisites
-------------

louse will only work on Linux. In order to build it, a C++11-enabled C++ 
compiler is needed (for example, g++ 4.9 will do). louse depends on
pthreads and libunwind, which must be installed before louse can be 
built. To resolve stacktraces, louse will also call `addr2line`,
which must be present in `/usr/bin` when louse is invoked.

louse generally can monitor any executable, but the stacktraces it
produces will not contain any useful information if the executable 
itself is compiled without debug symbols.

In order to get meaningful stacktraces, add the `-g` option to the
compiler flags when compiling an executable. For example, instead
of compiling like this

```bash
g++ test.cc -o test
```

try compiling like this:

```bash
g++ -g test.cc -o test
```

It is often useful to set the `-g` option in the `CXXFLAGS` and
`CFLAGS` environment variables, too.


Installation
------------

In order to install louse, you will need to have a C++11-enabled C++
compiler first.

When a C++ compiler is present, you need to install libunwind and the
addr2line binary.

To install libunwind on Ubuntu, use the following command:

```bash
sudo apt-get install libunwind-dev
```

To install addr2line on Ubuntu, use the following command:

```bash
sudo apt-get install binutils
```

The installation commands for other platforms may differ, but are not
covered here.

After cloning the louse repository from Github, execute the following
command to install the `louse` wrapper shell script and the `liblouse.so`
library:

```bash
sudo make install
```

The louse Makefile uses the following locations when installing:

* `/usr/bin` for the `louse` wrapper shell script
* `/usr/lib` for the `liblouse.so` library

If your system uses different locations, please adjust the `install`
rule in the Makefile accordingly. This should be trivial.


Usage
-----

To have louse track the memory operations of an executable, simply 
prepend the invocation command with `louse`.

For example, if the goal is to track the memory allocations and
deallocations of the command `myprog -s foo`, use `louse myprog -s foo`.


### Command-line options

louse provides the following options:

* `--with-leaks`: make louse report memory leaks at shutdown (requires
  the executable to terminate regularly and call exit handlers)
* `--with-traces`: capture stack traces for all memory allocations. 
  This has notable runtime overhead, but is required for retrieving 
  meaningful output in case of errors
* `--frames`: maximum number of stack frames to capture. Adjusting this
  value will have an effect on the memory consumption required for
  keeping stackfraces, and for the time required to produce them and
  report them at the end in case of memleaks.
* `--suppress`: a regular expression that can be used to suppress memory
  leaks if any line in their stack trace matches it. This can be used
  to suppress certain known leaks in libraries or otherwise unfixable
  leaks.


### Finding memory allocation and deallocation errors

Consider the following example program `myprog.cc`:

```cpp
int main () {
  int* a = new int[10];

  delete a;  // wrong! should use delete[] a!
}
```

Obviously the `delete` uses the wrong form, as the allocation took
place using a `new[]`. So we should rather use `delete[]` to free
the memory. Let's look at what louse has to say:

```bash
g++ -g myprog.cc -o myprog      # compile myprog
louse ./myprog                  # run myprog with louse
```

```
runtime error: trying to delete memory pointer 0xd62c50 that was originally allocated via new[]
  # main (myprog.cc:5)
  # __libc_start_main (/build/buildd/glibc-2.19/csu/libc-start.c:321)
  # _start (??:?)
```

Let's try it the other way around:
```cpp
int main () {
  int* a = new int;

  delete[] a;  // wrong: should use delete a!
}
```

Running the above program with louse results in:
```
runtime error: trying to delete[] memory pointer 0x1c2cc50 that was originally allocated via new
  # main (myprog.cc:5)
  # __libc_start_main (/build/buildd/glibc-2.19/csu/libc-start.c:321)
  # _start (??:?)
```

Note that louse will report memory errors only, but will not magically fix
your program. It is likely that your program will crash after a wrong deallocation.
It's not at all the goal of louse to prevent this.

A slightly more complex example, that uses both `malloc` and `new` for memory
allocation and messes it up:
```cpp
#include <cstdlib>

static void* getMemory (size_t size, bool useMalloc) {
  if (useMalloc) {
    return ::malloc(size);
  }
  return new void*;
}

int main () {
  void* a = getMemory(42, true);
  void* b = getMemory(23, false);

  ::free(a);
  ::free(b);  // wrong: should use delete b!
}
```
For the above example louse will report
```
runtime error: trying to free() memory pointer 0x825d90 that was originally allocated via new
  # main (myprog.cc:16)
  # __libc_start_main (/build/buildd/glibc-2.19/csu/libc-start.c:321)
  # _start (??:?)

original allocation site of memory pointer 0x825d90 via new:
  # getMemory (myprog.cc:8)
  # main (myprog.cc:12)
  # __libc_start_main (/build/buildd/glibc-2.19/csu/libc-start.c:321)
  # _start (??:?)
```

### Finding buffer overruns

louse does not monitor memory accesses in general, so it cannot detect the
usage of invalid memory locations. There is one exception: when the executable
returns memory via `free`, `delete` or `delete[]`, louse will perform a quick
consistency check at the end of the returned memory region. When the executable
allocates memory via louse, louse will write its signature directly after the
allocated memory block. If the signature on return is modified, this is a sure
indication of a buffer overrun, which louse will report.

Note that buffer overruns can only be detected by louse if the executable
returns the memory and does not crash before. However, many overruns are by
a few bytes only, and there are chances that they are still in the padding 
region of the memory returned by `malloc`. This may go unnoticed in many
situations with plain `malloc`.

Here's a test program for that:
```cpp
#include <cstdlib>
#include <cstdio>

int main (int argc, char* argv[]) {
  if (argc < 2) {
    ::printf("usage: %s value\n", argv[0]);
    ::exit(1);
  }

  int value = ::atoi(argv[1]);
  char* buffer = static_cast<char*>(::malloc(42));

  if (buffer) {
    for (int i = 0; i < value; ++i) {
      buffer[i] = 'x';  // overrun here if argv[1] is > 42!
    }
    ::free(buffer);
  }
}
```
Running the above program with an argument value of less than `43` is fine. 
However, running it with an argument value bigger than `43` will trigger a 
buffer overrun. Calling it with a value of `43` will produce:
```
runtime error: buffer overrun after memory pointer 0x1dd0c50 of size 42 that was originally allocated via malloc()
  # main (myprog.cc:19)
  # __libc_start_main (/build/buildd/glibc-2.19/csu/libc-start.c:321)
  # _start (??:?)

original allocation site of memory pointer 0x1dd0c50 via malloc():
  # main (myprog.cc:11)
  # __libc_start_main (/build/buildd/glibc-2.19/csu/libc-start.c:321)
  # _start (??:?)
```
If the buffer overrun is more drastic and longer than the length of the tail 
signature that is added to allocated memory blocks by louse, the executable
will likely crash, and louse will not have a chance to report issues.
Changing this would require tracking **all** memory accesses by the program,
which is far beyond the scope of louse. Use [Valgrind](http://valgrind.org) for that!

### Finding memory leaks

louse can also detect memory leaks in executables if they terminate regularly.
For example, the following program will leak the memory from two allocations:
```cpp
#include <cstdlib>
  
static void runProg () {
  void* a = ::malloc(23); 
  if (a) {
    char* b = new char[42];
    throw "some exception!";  // we'll leak b and a if we get here!
  }
}

int main () {
  try {
    runProg();
    return 0;
  }
  catch (...) { 
    return 1;
  }
}
```
When running the above program, louse will report:
```
check error: leak of size 42 byte(s), allocated with via new[]:
  # runProg (myprog.cc:6)
  # main (myprog.cc:14)
  # __libc_start_main (/build/buildd/glibc-2.19/csu/libc-start.c:321)
  # _start (??:?)

check error: leak of size 23 byte(s), allocated with via malloc():
  # runProg (myprog.cc:4)
  # main (myprog.cc:14)
  # __libc_start_main (/build/buildd/glibc-2.19/csu/libc-start.c:321)
  # _start (??:?)

check error: found 2 leaks(s) with total size of 65 byte(s)
```


### Leaks in libraries

Libraries sometimes allocate memory but never free it. Regardless of if
you consider this to be a bug or not, these leaks will be reported in 
the output of louse if the monitored executable uses such libraries.
louse does not distinguish between *real* memory leaks and *still reachable*
memory such as Valgrind does. For louse, all memory that is not freed
is simply a memory leak.

To suppress such well-known memory leaks from libraries that you have
no influence on, use the `--suppress` option of louse.

For example, to suppress the following leaks from the OpenSSL library
```
check error: leak of size 32 byte(s), allocated with via malloc():
  # CRYPTO_malloc (??:?)
  # sk_new (??:?)
  # PEM_write_SSL_SESSION (??:?)
  # SSL_COMP_get_compression_methods (??:?)
  # SSL_library_init (??:?)

check error: leak of size 1024 byte(s), allocated with via malloc():
  # strerror (/build/buildd/glibc-2.19/string/strerror.c:39)
  # ERR_load_ERR_strings (??:?)
  # ERR_load_crypto_strings (??:?)
  # SSL_load_error_strings (??:?)
```
you could use `--suppress "(CRYPTO_malloc|SSL_load_error_strings)"` when
invoking louse.


Runtime overhead
----------------

Monitoring an executable with louse is naturally more expensive than
running the executable standalone.

louse intercepts all calls to memory allocation and deallocation functions 
`malloc`, `calloc`, `realloc`, `free` and the C++ operators `new`,
`new[]`, `delete` and `delete[]`. When any of these operations is called
from the monitored executable, louse will intercept this call, track it
and then fall back to the function it originally intercepted.

This will make all memory allocation and deallocation functions a bit
slower. Additionally, each memory allocation will have an overhead of around 
64 bytes on x86_64. This is required for louse's memory bookkeeping. 

louse keeps allocations and deallocations in a linked list, which is 
protected by a simple mutex. This may cause contention for memory allocations
and deallocations in multi-threaded programs.

When louse is started with the `--with-traces=true` option (which is the default),
it will also create and store a stacktrace for each memory allocation.
This stacktrace will cost some additional memory (actual size depending on the
call stack depth) and also CPU cycles to construct.

Turning off stack traces (i.e. `--with-traces=false`) will result in a
notable speedup of the monitored executable and reduced memory consumption by
louse. Though louse will not produce any stack traces then, it can still 
be used to find memory deallocation errors and buffer overruns, and can be
used to check if the monitored program leaks.

Turning off stack traces will also greatly reduce the shutdown time of louse.


Limitations
-----------

louse is very limited. It has been hacked together in three nights, so
don't expect it to be perfect. It has lots of limitations. Here are a
few:

* louse will only work on Linux. It is unknown if it works on other
  platforms than Ubuntu.
* For meaningful stack traces, it is required to compile the monitored
  executable with debug symbols. Otherwise the stack traces will only
  contain lots of `??:?`.
* louse depends on `addr2line` to produce stacktraces, so the output of
  louse is only as good as the addr2line output.
* louse does not track memory accesses in general. Invalid memory accesses
  performed by the monitored executable will still crash the executable
  and louse, without louse having a chance to report this. It is 
  recommended to fall back to core dumps and gdb in this case.
* If the monitored executable does not terminate regularly and/or doesn't
  call the regular exit handlers, louse will not report anything at 
  shutdown.
* Only calls to `malloc`, `calloc`, `realloc`, `new` and `new[]` are
  intercepted. Executables that allocate memory via `brk`, `sbrk`, 
  `posix_memalign`, `aligned_alloc` or other means cannot be monitored with
  louse. `mmap` and `munmap` are not intercepted by louse either.
