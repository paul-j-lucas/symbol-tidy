/*
**      symbol-tidy -- Symbol tidier
**      src/lang.h
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

#ifndef tidy_lang_h
#define tidy_lang_h

/**
 * @file
 * Declares functions parsing strings into languages.
 */

// local
#include "pjl_config.h"                 /* must go first */

// libclang
#include <clang-c/Index.h>

// standard
#include <stdbool.h>

/**
 * @defgroup tidy-lang-group Languages
 * Functions parsing strings into languages.
 * @{
 */

////////// extern functions ///////////////////////////////////////////////////

/**
 * Gets the language corresponding to \a ext.
 *
 * @param ext A case-insensitive source filename extension (without the dot).
 * @return Returns the language corresponding to \a ext if found or
 * `CXLanguage_Invalid` if not.
 */
NODISCARD
enum CXLanguageKind lang_parse_ext( char const *ext );

/**
 * Gets the language corresponding to \a name.
 *
 * @param name The language name.  Must be one of `"c"`, `"c++"`, or `"none"`.
 * @param rv_lang If not NULL, receives the language corresponding to \a name.
 * @return Returns `true` only if \a name is valid.
 */
NODISCARD
bool lang_parse_name( char const *name, enum CXLanguageKind *rv_lang );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_lang_h */
/* vim:set et sw=2 ts=2: */
