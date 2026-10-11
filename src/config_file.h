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

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <stdbool.h>

/// @endcond

/**
 * @defgroup config-file-group Configuration File
 * Functions to read an **include-tidy** configuration file and look-up
 * information specified therein.
 * @{
 */

////////// extern variables ///////////////////////////////////////////////////

/**
 * Ignore \ref tidy_source_path?
 */
extern bool         tidy_config_is_source_path_ignored;

////////// extern functions ///////////////////////////////////////////////////

/**
 * Reads an **include-tidy** configuration file, if any.
 *
 * @note This function must be called at most once.
 */
void config_init( void );

/**
 * Gets whether \a sym_name should be ignored.
 *
 * @param sym_name The symbol name to check.
 * @return Returns `true` only if \a sym_name should be ignored.
 */
NODISCARD
bool config_symbol_is_ignored( char const *sym_name );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_config_file_h */
/* vim:set et sw=2 ts=2: */
