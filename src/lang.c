/*
**      symbol-tidy -- Symbol tidier
**      src/lang.c
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
**      along with this program.  If not, see <http
*/

/**
 * @file
 * Defines functions for parsing strings into languages.
 */

// local
#include "pjl_config.h"                 /* must go first */
#include "lang.h"
#include "util.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <stddef.h>
#include <strings.h>

/// @endcond

/**
 * @addtogroup tidy-lang-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct ext_map ext_map;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A source file extension and the language it corresponds to.
 */
struct ext_map {
  char const           *ext;            ///< Extension (without the `'.'`).
  enum CXLanguageKind   lang;           ///< Language for \ref ext.
};

////////// local constants ////////////////////////////////////////////////////

/**
 * Array of all common C/C++ filename extensions.
 */
static ext_map const EXT_MAP[] = {
  { "c",   CXLanguage_C         },
  { "c++", CXLanguage_CPlusPlus },
  { "cc",  CXLanguage_CPlusPlus },
  { "cp",  CXLanguage_CPlusPlus },
  { "cpp", CXLanguage_CPlusPlus },
  { "cxx", CXLanguage_CPlusPlus },
};

////////// extern functions ///////////////////////////////////////////////////

enum CXLanguageKind lang_parse_ext( char const *ext ) {
  assert( ext != NULL );

  FOREACH_ARRAY_ELEMENT( ext_map, map, EXT_MAP ) {
    if ( strcasecmp( ext, map->ext ) == 0 )
      return map->lang;
  } // for

  return CXLanguage_Invalid;
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
