/*
**      symbol-tidy -- Symbol tidier
**      src/clang_util.h
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

#ifndef tidy_clang_util_h
#define tidy_clang_util_h

/**
 * @file
 * Declares utility functions for libclang.
 */

// local
#include "pjl_config.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <stdbool.h>
#include <stddef.h>

/// @endcond

/**
 * @defgroup clang-util-group libclang Utility Functions
 * Utility functions for libclang.
 * @{
 */

////////// extern functions ///////////////////////////////////////////////////

/**
 * Given a cursor at a local name of an enumeration, class, class data member,
 * class member function, structure, union, or namespace, gets its fully scoped
 * "display" name that includes template parameters (if any).
 *
 * @param cursor The cursor for a symbol.
 * @return Returns the fully scoped name.
 *
 * @note The caller is responsible for freeing the returned string.
 */
NODISCARD
char* tidy_Cursor_getScopedDisplayName( CXCursor cursor );

/**
 * Gets whether \a cursor is a class, class template, structure, or union
 * declaration.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor is a class, class template,
 * structure, or union declaration.
 *
 * @sa tidy_Cursor_isScopeDecl()
 */
NODISCARD
bool tidy_Cursor_isClassDecl( CXCursor cursor );

/**
 * Gets whether \a cursor is an external, global function.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor is an external, global function.
 */
NODISCARD
bool tidy_Cursor_isExternalGlobalFunction( CXCursor cursor );

/**
 * Gets whether \a cursor has external linkage.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor has external linkage.
 */
NODISCARD
bool tidy_Cursor_isExternalLinkage( CXCursor cursor );

/**
 * Gets whether a class, data member, or member function given by \a cursor is
 * derived, inherited, or specialized from the class given by \a base_csr.
 *
 * @param cursor The candidate cursor.
 * @param base_csr The candidate base class cursor.
 * @return Returns `true` only if \a cursor is inherited from \a base_csr.
 */
NODISCARD
bool tidy_Cursor_isInheritedFrom( CXCursor cursor, CXCursor base_csr );

#ifdef NEED_tidy_Cursor_isInheritedMemberFunctionCall
// All code guarded by NEED_tidy_Cursor_isInheritedMemberFunctionCall is code
// was presumably needed, but removing it causes no tests to fail, so it may
// not be needed.  But I didn't want to delete the code (even though it still
// would be in the git repo) just in case it turns out to be necessary after
// all at some point.

/**
 * Gets whether a C++ member function is inherited from a base class.
 *
 * @par Example
 * @parblock
 * Given something like:
 *
 *      struct Base {
 *        void f();
 *      };
 *
 *      struct Derived : Base {
 *        // ...
 *      };
 *
 *      void g() {
 *        Derived d;
 *        d.f();                        // f() is inherited from Base
 *        // ...
 *      }
 *
 * the member function `f()` isn't declared in `Derived`, but declared in and
 * inherited from `Base`.
 *
 * @endparblock
 *
 * @param expr_csr The expression the member function is being called on.
 * @param rv_cls_csr If not NULL and the member function was inherited,
 * receives the cursor for the base class that the function was declared in.
 * @return Returns `true` only if the member function was inherited.
 */
NODISCARD
bool tidy_Cursor_isInheritedMemberFunctionCall( CXCursor expr_csr,
                                                CXCursor *rv_cls_csr );
#endif /* NEED_tidy_Cursor_isInheritedMemberFunctionCall */

/**
 * Gets whether \a cursor is either null or invalid.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor is either null or invalid.
 */
NODISCARD
inline bool tidy_Cursor_isInvalid( CXCursor cursor ) {
  return clang_Cursor_isNull( cursor ) || clang_isInvalid( cursor.kind );
}

/**
 * Gets whether \a cursor has a name that is reserved in \a lang.
 *
 * @remarks A name is reserved if it matches any of these patterns:
 *
 *      _*          // C: external only; C++: global namespace only.
 *      _[A-Z_]*    // Both C and C++.
 *      *__*        // C++ only.
 *
 * @param cursor The cursor to check
 * @param lang The language to use.
 * @return Returns `true` only if \a cursor has a name that is reserved in \a
 * lang.
 */
NODISCARD
bool tidy_Cursor_isReservedName( CXCursor cursor, enum CXLanguageKind lang );

/**
 * Gets whether \a cursor is a class, class template, enumeration, namespace,
 * structure, or union declaration.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor is a class, class template,
 * enumeration, namespace, structure, or union.
 *
 * @sa tidy_Cursor_isClassDecl()
 */
NODISCARD
bool tidy_Cursor_isScopeDecl( CXCursor cursor );

/**
 * Gets whether \a cursor is a template specialization of \a template_csr.
 *
 * @param cursor The candicate template class specialization cursor.
 * @param template_csr The candidate template class cursor.
 * @return Returns `true` only if \a template_csr is a template class and \a
 * cursor is a specialization of it.
 */
NODISCARD
bool tidy_Cursor_isTemplateSpecializationOf( CXCursor cursor,
                                             CXCursor template_csr );

/**
 * Gets the real path of \a file.
 *
 * @param file The file to get the real path of.
 * @return Returns the string containing the real path of \a file.  The caller
 * _must_ call `clang_disposeString()` on it.
 *
 */
NODISCARD
CXString tidy_File_getRealPathName( CXFile file );

/**
 * Calls `clang_getCursorUSR()` on the canonical cursor of \a cursor.
 *
 * @param cursor The cursor to use.
 * @return Returns the result of `clang_getCursorUSR()` on the canonical cursor
 * of \a cursor.
 *
 * @note The caller is responsible for freeing the returned string.
 */
NODISCARD
char* tidy_getCursorUSR( CXCursor cursor );

/**
 * Attempts to get the cursor for the identifier having \a name within \a
 * scope_csr.
 *
 * @param name The name to get the cursor for.
 * @param scope_csr The scope to look in.
 * @return Returns said cursor or an invalid cursor if not found.
 *
 * @sa tidy_getCursorByNameToken()
 */
NODISCARD
CXCursor tidy_getCursorByName( char const *name, CXCursor scope_csr );

/**
 * Gets the cursor for the identifier given by \a token within \a scope_csr,
 * but only if \a token actually is an identifier.
 *
 * @param tu The translation unit to use.
 * @param token The token to get the cursor for.
 * @param scope_csr The cursor of the scope to search within.
 * @return Returns said cursor; or an invalid cursor if \a token is an
 * identifier, but not found; or the null cursor if \a token is not an
 * identifier.
 *
 * @sa tidy_getCursorByName()
 */
NODISCARD
CXCursor tidy_getCursorByNameToken( CXTranslationUnit tu, CXToken token,
                                    CXCursor scope_csr );

/**
 * Similar to `clang_getCursorExtent()` except that it works better when macros
 * are involved.
 *
 * @par Example
 * @parblock
 * Given something like:
 *
 *      #include <stdalign.h>           // alignas
 *      #include <stddef.h>             // max_align_t
 *
 *      struct rb_node {
 *        // ...
 *        alignas( max_align_t ) char data[];
 *      };
 *
 * where `alignas` is a macro defined in `stdalign.h`, libclang never visits
 * the macro.  Furthermore, attempting to tokenize the extent of the FieldDecl
 * using clang_getCursorExtent() does _not_ return any tokens because libclang
 * can't tokenize a contiguous range across file boundaries (`alignas` is in
 * `stdalign.h` and the rest is in the file being tidied).
 *
 * However, _this_ function can do such a tokenization.
 * @endparblock
 *
 * @param cursor The cursor to get the source range for.
 * @return Returns said source range.
 */
NODISCARD
CXSourceRange tidy_getCursorExtent( CXCursor cursor );

/**
 * Calls `clang_getCursorLocation()` and returns only the file.
 *
 * @param cursor The cursor to use.
 * @return Returns the file \a cursor is in, if any.
 */
NODISCARD
CXFile tidy_getCursorLocation_File( CXCursor cursor );

/**
 * Calls `clang_getFileLocation()` and returns the `CXFile`.
 *
 * @param loc The `CXSourceLocation` to use.
 * @return Returns its `CXFile`.
 *
 * @sa tidy_getSpellingLocation_File()
 */
NODISCARD
inline CXFile tidy_getFileLocation_File( CXSourceLocation loc ) {
  CXFile file;
  clang_getFileLocation(
    loc, &file, /*line=*/NULL, /*column=*/NULL, /*offset=*/NULL
  );
  return file;
}

/**
 * Calls `clang_getSpellingLocation()` and returns only the file.
 *
 * @param loc The location to use.
 * @return Returns the file of \a loc.
 *
 * @sa tidy_getFileLocation_File()
 * @sa tidy_getSpellingLocation_Line()
 */
NODISCARD
inline CXFile tidy_getSpellingLocation_File( CXSourceLocation loc ) {
  CXFile file;
  clang_getSpellingLocation(
    loc, &file, /*line=*/NULL, /*column=*/NULL, /*offset=*/NULL
  );
  return file;
}

/**
 * Calls `clang_getSpellingLocation()` and returns only the line.
 *
 * @param loc The location to use.
 * @return Returns the line of \a loc.
 *
 * @sa tidy_getSpellingLocation_File()
 */
NODISCARD
inline unsigned tidy_getSpellingLocation_Line( CXSourceLocation loc ) {
  unsigned line;
  clang_getSpellingLocation(
    loc, /*file=*/NULL, &line, /*column=*/NULL, /*offset=*/NULL
  );
  return line;
}

/**
 * Calls `clang_getTokenLocation()` and returns only the line.
 *
 * @param tu The `CXTranslationUnit` to use.
 * @param token The `CXToken` to use.
 * @return Returns the line of \a token.
 */
NODISCARD
inline unsigned tidy_getTokenLocation_Line( CXTranslationUnit tu,
                                            CXToken token ) {
  CXSourceLocation const loc = clang_getTokenLocation( tu, token );
  unsigned line;
  clang_getSpellingLocation(
    loc, /*file=*/NULL, &line, /*column=*/NULL, /*offset=*/NULL
  );
  return line;
}

/**
 * Calls `clang_isCursorDefinition()` but, in C, also considers tentative
 * definitions as definitions.
 *
 * @remarks
 * @parblock
 * In C, a defintion that does not have an explicit initializer is a "tentative
 * definition":
 *
 *      int a = 0;  // definition
 *      int b;      // tentative definition
 *
 * But we want to consider tentative definitions as real definitions.
 * @endparblock
 *
 * @param cursor The cursor to check.
 * @return Returns `true` if \a cursor is either a definition or, in C, a
 * tentative definition.
 */
NODISCARD
bool tidy_isCursorDefinition( CXCursor cursor );

/**
 * Gets a pointer to the next non-comment token in \a tokens, if any.
 *
 * @param tokens The array of tokens.
 * @param token_count The length of \a tokens.
 * @param ptoken_idx A pointer to the current token index.  It is updated to be
 * either the index of the next non-comment token or \a token_count if none.
 * @return Returns a pointer to the next non-comment token or NULL for none.
 */
NODISCARD
CXToken const* tidy_Token_getNext( CXToken const tokens[], unsigned token_count,
                                   unsigned *ptoken_idx );

/**
 * Gets whether the spelling of \a token equals \a value.
 *
 * @param tu The translation unit to use.
 * @param token The token to compare.
 * @param value The value to compare against.
 * @return Returns `true` only if the spelling of \a token equals \a value.
 *
 * @sa #tidy_Token_isEqualToAny()
 * @sa tidy_Token_isScopeQualifier()
 */
NODISCARD
bool tidy_Token_isEqualTo( CXTranslationUnit tu, CXToken token,
                           char const *value );

/**
 * Gets whether the spelling of \a token equals any one of \a values.
 *
 * @param TU The translation unit to use.
 * @param TOKEN The token to compare.
 * @param ... A comma separated list of values to compare against.  It is
 * implicitly NULL terminated.
 * @return Returns the index of the matched value or -1 for none.
 *
 * @sa tidy_Token_isEqualTo()
 */
#define tidy_Token_isEqualToAny(TU,TOKEN,...)   \
  tidy_Token_isEqualToAny_impl( (TU), (TOKEN),  \
                                (char const*[]){ __VA_ARGS__, NULL } )

/**
 * Gets whether the spelling of \a token equals any one of \a values.
 *
 * @note This function isn't normally called directly; use the
 * #tidy_Token_isEqualToAny() macro instead.
 *
 * @param tu The translation unit to use.
 * @param token The token to compare.
 * @param values The NULL terminated array of values to compare against.
 * @return Returns the index of the matched value or -1 for none.
 *
 * @sa tidy_Token_isEqualTo()
 */
NODISCARD
int tidy_Token_isEqualToAny_impl( CXTranslationUnit tu, CXToken token,
                                  char const *values[] );

/**
 * Gets whether the spelling of \a token is the scope qualifier `"::"`.
 *
 * @param tu The translation unit to use.
 * @param token The token to check.
 * @return Returns `true` only if the spelling of \a token equals `"::"`.
 * @sa tidy_Token_isEqualTo()
 */
NODISCARD
bool tidy_Token_isScopeQualifier( CXTranslationUnit tu, CXToken token );

///////////////////////////////////////////////////////////////////////////////

/** @} */

#endif /* tidy_clang_util_h */
/* vim:set et sw=2 ts=2: */
