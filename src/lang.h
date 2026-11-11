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
 * @defgroup tidy-lang-group File Extensions
 * A structure and functions for supported C/C++ filename extensions.
 * @{
 */

////////// extern functions ///////////////////////////////////////////////////

/**
 * Attempts to find \a ext among the set of supported C/C++ file extensions.
 *
 * @param ext A filename extension (without the dot).
 * @return Returns the corresponding language if found or `CXLanguage_Invalid`
 * if not.
 */
NODISCARD
enum CXLanguageKind lang_parse_ext( char const *ext );

/**
 * Attempts to find \name among the set of supported language names.
 *
 * @param name The name to parse.  Must be one of `"c"`, `"c++"`, or `"none"`.
 * @param rv_lang If not NULL, receives the language corresponding to \a name.
 * @return Returns `true` only if \a name is valid.
 */
NODISCARD
bool lang_parse_name( char const *name, enum CXLanguageKind *rv_lang );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_lang_h */
/* vim:set et sw=2 ts=2: */
