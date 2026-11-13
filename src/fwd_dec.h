/*
**      symbol-tidy -- Symbol tidier
**      src/fwd_dec.h
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

#ifndef tidy_fwd_dec_h
#define tidy_fwd_dec_h

/**
 * @file
 * Declares functions for finding and printing unnecessary forward function
 * declarations within a single translation unit.
 */

// local
#include "pjl_config.h"
#include "source.h"
#include "symbol.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

/// @endcond

/**
 * @defgroup tidy-fwd-dec-group Forward Declarations
 * Functions for finding unnecessary forward function declarations.
 *
 * @remarks
 * @parblock
 * A forward declaration of a function is unnecessary if it's not referenced
 * anywhere between its declaration and definition in the same file.
 *
 * Unlike unused symbols, unnecessary forward declarations are reported on a
 * per-translation unit basis since they can't span translation units.
 * @endparblock
 * @{
 */

////////// extern functions ///////////////////////////////////////////////////

/**
 * Records \a fn_csr as a candidate unnecessary forward declaration, but only
 * if it passes all the filters.
 *
 * @param fn_csr The cursor for a `CXCursor_FunctionDecl` that is _not_ a
 * definition.
 * @param svd The symbols_visit_data to use.
 *
 * @sa fwd_dec_visit_ref()
 */
void fwd_dec_maybe_unnecessary( CXCursor fn_csr,
                                symbols_visit_data const *svd );

/**
 * Records the definition of a function.  If the function is a candidate,
 * remembers where its definition is.
 *
 * @param fn_csr The cursor for a function definition.
 * @param svd The symbols_visit_data to use.
 *
 * @sa fwd_dec_maybe_unnecessary()
 * @sa fwd_dec_visit_ref()
 */
void fwd_dec_visit_def( CXCursor fn_csr, symbols_visit_data const *svd );

/**
 * Notes a reference to a function.  If the function is a candidate and this is
 * its first reference, remembers where.
 *
 * @param ref_csr The cursor of the function referenced.
 * @param svd The symbols_visit_data to use.
 *
 * @sa fwd_dec_visit_def()
 */
void fwd_dec_visit_ref( CXCursor ref_csr, symbols_visit_data const *svd );

/**
 * Initializes forward declarations.
 *
 * @note This function must be called exactly once.
 */
void fwd_decs_init( void );

/**
 * Prints all unnecessary forward declarations found in the current translation
 * unit, then discards all state for it.
 *
 * @param source The tidy_source for the translation unit.
 * @return Returns the number of unnecessary forward declarations for \a source.
 */
NODISCARD
unsigned fwd_decs_print_unnecessary( tidy_source const *source );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_fwd_dec_h */
/* vim:set et sw=2 ts=2: */
