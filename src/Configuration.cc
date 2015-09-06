
#include <cstring>
#include <cstdlib>
#include <string>

#include "Configuration.h"

using Configuration = debugging::Configuration;

// -----------------------------------------------------------------------------
// --SECTION--                                              struct Configuration
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief create the configuration with defaults
////////////////////////////////////////////////////////////////////////////////

Configuration::Configuration ()
  : suppressFilter(nullptr), withLeaks(true), withTraces(true), maxFrames(16), maxLeaks(100) {
} 

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief initialize the configuration from environment variables
////////////////////////////////////////////////////////////////////////////////

void Configuration::fromEnvironment () {
  char const* value;

  value = ::getenv("LOUSE_WITHLEAKS");

  if (value != nullptr) {
    withLeaks = toBoolean(value, withLeaks);
  }

  value = ::getenv("LOUSE_WITHTRACES");

  if (value != nullptr) {
    withTraces = toBoolean(value, withTraces);
  }

  value = ::getenv("LOUSE_FILTER");

  if (value != nullptr) {
    suppressFilter = value;
  }

  value = ::getenv("LOUSE_MAXFRAMES");

  if (value != nullptr) {
    maxFrames = toNumber(value, maxFrames);
  }

  value = ::getenv("LOUSE_MAXLEAKS");

  if (value != nullptr) {
    maxLeaks = toNumber(value, maxLeaks);
  }
}

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

////////////////////////////////////////////////////////////////////////////////
/// @brief convert a string argument to a boolean
////////////////////////////////////////////////////////////////////////////////

bool Configuration::toBoolean (char const* value, bool defaultValue) const {
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

////////////////////////////////////////////////////////////////////////////////
/// @brief convert a string argument to a number
////////////////////////////////////////////////////////////////////////////////

int Configuration::toNumber (char const* value, int defaultValue) const {
  try {
    int v = std::stol(value);

    if (v < 1) {
      v = 1;
    }

    return v;
  }
  catch (...) {
    return defaultValue;
  }
}

