/*
**      symbol-tidy -- Symbol tidier
**      src/source.h
**
**      Copyright (C) 2026  Paul J. Lucas, et al.
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

#ifndef tidy_source_h
#define tidy_source_h

/**
 * @file
 * Declares a structure and functions for a source file being tidied.
 */

// local
#include "pjl_config.h"
#include "array.h"

// libclang
#include <clang-c/Index.h>

// standard
#include <stdbool.h>

/**
 * @defgroup tidy-source-group Source File
 * A structure for a source file being tidied.
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct tidy_source tidy_source;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A source file being tidied.
 */
struct tidy_source {
  char const         *argv_path;        ///< Source file's path from `argv`.
  CXFile              file;             ///< The file fir \ref argv_path.
  unsigned            tsid;             ///< Unique source ID.
  enum CXLanguageKind lang;             ///< Language of source file.

  /**
   * Points to an \ref ptu_argv_global "array" of command-line arguments to
   * pass to `clang_parseTranslationUnit2` corresponding to \ref lang.
   */
  array_t const      *ptu_argv;

  CXTranslationUnit   tu;               ///< Translation unit.
};

////////// extern functions ///////////////////////////////////////////////////

/**
 * Gets whether \a path is among the source paths given on the command line.
 *
 * @param path The path to check.
 * @return Returns `true` only if \a path is among the source paths given on
 * the command line.
 */
NODISCARD
bool is_source_path( char const *path );

/**
 * Cleans-up all memory associated with \a source but does _not_ free \a source
 * itself.
 *
 * @param source the tidy_source to clean up.  If NULL, does nothing.
 */
void source_cleanup( tidy_source *source );

/**
 * Parses \a source.
 *
 * @param source The source to parse.
 */
void source_parse( tidy_source *source );

/**
 * Adds \a path to the set of source files being tidied.
 *
 * @param path The path to add.
 * @return Returns `true` only if \a path was added; `false` only if it was
 * already in the set.
 */
bool source_path_add( char const *path );

/**
 * Initializes sources.
 *
 * @note This function must be called exactly once.
 */
void sources_init( void );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_source_h */
/* vim:set et sw=2 ts=2: */
