
#include <unistd.h>

#include "Printer.h"

using Printer = debugging::Printer;

// -----------------------------------------------------------------------------
// --SECTION--                                                     class Printer
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief emits a line
////////////////////////////////////////////////////////////////////////////////

void Printer::EmitLine (FILE* out, char const* format, ...) { 
  char buffer[2048];

  va_list ap;
  va_start(ap, format);
  int length = ::vsnprintf(buffer, sizeof(buffer) - 1, format, ap);
  va_end(ap);

  buffer[sizeof(buffer) - 1] = '\0'; // paranoia

  if (length >= 0) {
    ::fprintf(out, "%s\n", &buffer[0]);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief emits an error
////////////////////////////////////////////////////////////////////////////////

void Printer::EmitError (FILE* out, char const* type, char const* format, ...) { 
  char buffer[2048];

  va_list ap;
  va_start(ap, format);
  int length = ::vsnprintf(buffer, sizeof(buffer) - 1, format, ap);
  va_end(ap);

  buffer[sizeof(buffer) - 1] = '\0'; // paranoia

  if (length > 0) {
    if (UseColors(out)) {
      ::fprintf(out, "\n\033[31;1m%s error: %s\033[0m\n", type, &buffer[0]);
    }
    else {
      ::fprintf(out, "\n%s error: %s\n", type, &buffer[0]);
    }
    ::fflush(out);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief whether or not we should use color printing
////////////////////////////////////////////////////////////////////////////////

bool Printer::UseColors (FILE* out) {
  return (::isatty(::fileno(out)) == 1);
}

