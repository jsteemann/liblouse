
#ifndef LOUSE_CONFIGURATION_H
#define LOUSE_CONFIGURATION_H 1

// -----------------------------------------------------------------------------
// --SECTION--                                              struct Configuration
// -----------------------------------------------------------------------------

namespace debugging {
  struct Configuration {

// -----------------------------------------------------------------------------
// --SECTION--                                        constructors / destructors
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief create the configuration with defaults
////////////////////////////////////////////////////////////////////////////////

      Configuration ();

// -----------------------------------------------------------------------------
// --SECTION--                                                    public methods
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief initialize the configuration from environment variables
////////////////////////////////////////////////////////////////////////////////

      void fromEnvironment ();

// -----------------------------------------------------------------------------
// --SECTION--                                                   private methods
// -----------------------------------------------------------------------------

    private:

////////////////////////////////////////////////////////////////////////////////
/// @brief convert a string argument to a boolean
////////////////////////////////////////////////////////////////////////////////

      bool toBoolean (char const*, bool) const;

////////////////////////////////////////////////////////////////////////////////
/// @brief convert a string argument to a number
////////////////////////////////////////////////////////////////////////////////

      int toNumber (char const*, int) const;

// -----------------------------------------------------------------------------
// --SECTION--                                                  public variables
// -----------------------------------------------------------------------------

    public:

////////////////////////////////////////////////////////////////////////////////
/// @brief configuration value `--suppress`
////////////////////////////////////////////////////////////////////////////////

      char const*       suppressFilter;

////////////////////////////////////////////////////////////////////////////////
/// @brief configuration value `--with-leaks`
////////////////////////////////////////////////////////////////////////////////

      bool              withLeaks;

////////////////////////////////////////////////////////////////////////////////
/// @brief configuration value `--with-traces`
////////////////////////////////////////////////////////////////////////////////

      bool              withTraces;

////////////////////////////////////////////////////////////////////////////////
/// @brief configuration value `--max-frames`
////////////////////////////////////////////////////////////////////////////////

      int               maxFrames;

////////////////////////////////////////////////////////////////////////////////
/// @brief configuration value `--max-leaks`
////////////////////////////////////////////////////////////////////////////////

      int               maxLeaks;

  };
}

#endif