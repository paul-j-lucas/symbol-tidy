/*
**      symbol-tidy -- Symbol tidier
**      src/config_file.h
**
**      Copyright (C) 2026  Paul J. Lucas
**
**      This program is free software: you can redistribute it and/or modify
**      it under the terms of the GNU General Public License as published by
**      the Free Software Foundation, either version 3 of the License, or
**      (at your option) any later version.
**
**      This program is distributed in the hope that it will be useful,
**      but WITHOUT ANY WARRANTY; without even the implied warranty of
**      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**      GNU General Public License for more details.
**
**      You should have received a copy of the GNU General Public License
**      along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef tidy_config_file_h
#define tidy_config_file_h

/**
 * @file
 * Declares functions for reading and querying a **symbol-tidy** configuration
 * file.
 */

// local
#include "pjl_config.h"
#include "hash_table.h"

/// @cond DOXYGEN_IGNORE

// standard
#include <stdbool.h>

/// @endcond

/**
 * @defgroup config-file-group Configuration File
 * Functions to read an **symbol-tidy** configuration file and look-up
 * information specified therein.
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct config_bool config_bool;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A configuration file Boolean value.
 *
 * @remarks
 * @parblock
 * A `config_bool` is like a built-in `bool` except it can have per-path
 * overrides.  For example, a configuration file that contains:
 *
 *      [symbol-tidy]
 *      ignore-cxx-copy-assigns = true
 *
 *      [foo.c]
 *      ignore-cxx-copy-assign = false
 *
 * says that the global value for `ignore-cxx-copy-assigns` is `true`, but for
 * `foo.c`, it's `false`.
 * @endparblock
 */
struct config_bool {
  bool          global_value;           // The global value.
  hash_table_t  bool_path_set;          // Path-specific value(s).
};

////////// extern functions ///////////////////////////////////////////////////

/**
 * Cleans-up a config_bool.
 *
 * @param cb The config_bool to clean-up.  If NULL, does nothing.
 *
 * @sa config_bool_init()
 */
void config_bool_cleanup( config_bool *cb );

/**
 * Initializes a config_bool.
 *
 * @param cb The config_bool to initialize.
 *
 * @sa config_bool_cleanup()
 */
void config_bool_init( config_bool *cb );

/**
 * Gets the value of a config_bool.
 *
 * @param cb The config_bool to get the value of.
 * @param abs_path The source absolute path, if any, to get the value for.
 * @return Returns the Boolean value for \a abs_path, if any, or the global
 * value, if not.
 */
NODISCARD
bool config_bool_value( config_bool *cb, char const *abs_path );

/**
 * Reads an **symbol-tidy** configuration file, if any.
 *
 * @note This function must be called at most once.
 */
void config_init( void );

/**
 * Gets whether \a path should be ignored.
 *
 * @param path The path to check.
 * @return Returns `true` only if \a path should be ignored.
 */
NODISCARD
bool config_path_is_ignored( char const *path );

/**
 * Gets whether \a sym_name should be ignored.
 *
 * @param sym_name The symbol name to check.
 * @param sym_path The path containing \a sym_name.
 * @return Returns `true` only if \a sym_name should be ignored.
 */
NODISCARD
bool config_symbol_is_ignored( char const *sym_name, char const *sym_path );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_config_file_h */
/* vim:set et sw=2 ts=2: */
