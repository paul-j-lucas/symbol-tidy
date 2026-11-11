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
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>

/// @endcond

/**
 * @addtogroup tidy-lang-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct lang_map lang_map;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A source file extension and the language it corresponds to.
 */
struct lang_map {
  union {
    char const         *ext;            ///< Extension (without the `'.'`).
    char const         *name;           ///< Name: `"c"`, `"c++"`, or `"none"`.
  };
  enum CXLanguageKind   lang;           ///< Language for \ref ext.
};

////////// local constants ////////////////////////////////////////////////////

/**
 * Map of all common C/C++ filename extensions.
 */
static lang_map const EXT_MAP[] = {
  { .ext = "c",   CXLanguage_C         },
  { .ext = "c++", CXLanguage_CPlusPlus },
  { .ext = "cc",  CXLanguage_CPlusPlus },
  { .ext = "cp",  CXLanguage_CPlusPlus },
  { .ext = "cpp", CXLanguage_CPlusPlus },
  { .ext = "cxx", CXLanguage_CPlusPlus },
};

/**
 * Map of language names.
 */
static lang_map const NAME_MAP[] = {
  { .name = "c",    CXLanguage_C          },
  { .name = "c++",  CXLanguage_CPlusPlus  },
  { .name = "none", CXLanguage_Invalid    }
};

////////// extern functions ///////////////////////////////////////////////////

enum CXLanguageKind lang_parse_ext( char const *ext ) {
  assert( ext != NULL );

  FOREACH_ARRAY_ELEMENT( lang_map, map, EXT_MAP ) {
    if ( strcasecmp( ext, map->ext ) == 0 )
      return map->lang;
  } // for

  return CXLanguage_Invalid;
}

bool lang_parse_name( char const *name, enum CXLanguageKind *rv_lang ) {
  assert( name != NULL );

  FOREACH_ARRAY_ELEMENT( lang_map, map, NAME_MAP ) {
    if ( strcmp( name, map->name ) == 0 ) {
      if ( rv_lang != NULL )
        *rv_lang = map->lang;
      return true;
    }
  } // for

  return false;
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
