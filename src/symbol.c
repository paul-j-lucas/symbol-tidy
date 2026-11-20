/*
**      symbol-tidy -- Symbol tidier
**      src/symbol.c
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

/**
 * @file
 * Defines structures and functions for keeping track of symbols referenced.
 */

// local
#include "pjl_config.h"
#include "symbol.h"
#include "array.h"
#include "clang_util.h"
#include "color.h"
#include "config_file.h"
#include "cxx_symbol.h"
#include "fnv1a.h"
#include "fwd_dec.h"
#include "hash_table.h"
#include "options.h"
#include "path_util.h"
#include "source.h"
#include "str_util.h"
#include "symbol-tidy.h"
#include "util.h"
#include "verbose.h"

/// @cond DOXYGEN_IGNORE

// libclang
#include <clang-c/Index.h>

// standard
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>                     /* for unreachable(3) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/// @endcond

/**
 * @addtogroup tidy-symbols-group
 * @{
 */

////////// typedefs ///////////////////////////////////////////////////////////

typedef struct tidy_header        tidy_header;
typedef struct tidy_header_stats  tidy_header_stats;

////////// structs ////////////////////////////////////////////////////////////

/**
 * A header included in a translation unit.
 */
struct tidy_header {
  CXFileUniqueID      fuid;             ///< The file's unique ID.
  enum CXLanguageKind lang;             ///< From tidy_source.
  unsigned            tsid;             ///< From tidy_source.
};

/**
 * Additional statistics for the set of headers that were included across all
 * translation units.
 */
struct tidy_header_stats {
  unsigned  revisit_count;              ///< Headers visited more than once.
  unsigned  skipped_count;              ///< Headers skipped.
};

////////// local functions ////////////////////////////////////////////////////

static void symbol_update_macro_name_loc( tidy_symbol*, CXFile,
                                          symbols_visit_data const* );

static void visit_Constructor( CXCursor, symbols_visit_data* );
static void visit_CXXDeleteExpr( CXCursor, symbols_visit_data* );
static void visit_CXXForRangeStmt( CXCursor, symbols_visit_data* );
static void visit_CXXMethod( CXCursor, symbols_visit_data* );
static void visit_CXXNewExpr( CXCursor, symbols_visit_data* );
static void visit_FieldDecl( CXCursor, symbols_visit_data* );
static void visit_FunctionDecl( CXCursor, symbols_visit_data* );
static void visit_MacroDefinition( CXCursor, symbols_visit_data* );
static void visit_most_Ref( CXCursor, symbols_visit_data* );
static void visit_OverloadedDeclRef( CXCursor, symbols_visit_data* );
static void visit_VarDecl( CXCursor, symbols_visit_data* );

////////// extern variables ///////////////////////////////////////////////////

/// @cond DOXYGEN_IGNORE
/// Otherwise Doxygen generates two entries.

hash_table_t tidy_symbol_set;

/// @endcond

////////// local variables ////////////////////////////////////////////////////

static hash_table_t       header_set;   ///< The set of all header files.
static tidy_header_stats  header_stats; ///< Statistics for \ref header_set.
static bool               print_undefined;
static bool               print_unneeded_external;

////////// inline functions ///////////////////////////////////////////////////

/**
 * Gets whether \a sym was defined via a macro.
 *
 * @param sym The tidy_symbol to check.
 * @return Returns `true` only if \a sym was defined via a macro.
 */
NODISCARD
static inline bool symbol_is_def_in_macro( tidy_symbol const *sym ) {
  return sym->macro.spelling_loc.line > 0;
}

/**
 * Gets whether \a sym is referenced by another symbol in at least one
 * translation unit.
 *
 * @param sym The tidy_symbol to check.
 * @return Returns `true` only if \a sym is referenced.
 */
NODISCARD
static inline bool symbol_is_referenced( tidy_symbol const *sym ) {
  return sym->ext_link.is_ref_from_other_file || sym->ext_link.ref_tsid != 0;
}

/**
 * Gets whether \a sym is undefined and should be reported as such, i.e., it's
 * declared in a source path, but not defined.
 *
 * @param sym The tidy_symbol to check.
 * @return Returns `true` only if \a sym is undefined.
 */
NODISCARD
static inline bool symbol_is_undefined( tidy_symbol const *sym ) {
  return !sym->is_defined && sym->dec_loc.path != NULL;
}

/**
 * Gets whether \a sym is unused and should be reported as such.
 *
 * @remarks This can be called only after \ref tidy_symbol::is_dead "is_dead"
 * has been finalized for \a sym by symbols_mark_used().
 */
NODISCARD
static inline bool symbol_is_unused( tidy_symbol const *sym ) {
  return !sym->is_used && !sym->is_dead && !symbol_is_undefined( sym );
}

/**
 * Gets the name of \a sym that should be used for printing.
 *
 * @remarks For C, it's just the symbol's plain name; for C++, it's the
 * symbol's prototype so overloaded functions can be distinguished.
 *
 * @param sym The tidy_symbol to get the name of.
 * @return Returns said name.
 */
NODISCARD
static inline char const* symbol_name_for_print( tidy_symbol const *sym ) {
  return sym->cxx_prototype != NULL ? sym->cxx_prototype : sym->name;
}

////////// local functions ////////////////////////////////////////////////////

/**
 * Gets whether \a sym_csr is a definition of the kind of symbol that could be
 * given internal linkage: a non-member, non-template function or variable that
 * currently has external linkage.
 *
 * @param sym_csr The definition's cursor to check.
 * @return Returns `true` only if \a sym_csr could be `static`.
 */
NODISCARD
static bool cursor_is_maybe_internal( CXCursor sym_csr ) {
  enum CXCursorKind const kind = clang_getCursorKind( sym_csr );
  switch ( kind ) {
    case CXCursor_FunctionDecl:
    case CXCursor_VarDecl:
      break;
    default:
      return false;
  } // switch

  if ( !tidy_Cursor_isExternalLinkage( sym_csr ) )
    return false;                       // already static

  CXCursor const parent_csr = clang_getCursorSemanticParent( sym_csr );
  if ( tidy_Cursor_isClassDecl( parent_csr ) )
    return false;                       // static data member

  CXCursor const tmpl_csr = clang_getSpecializedCursorTemplate( sym_csr );
  if ( !tidy_Cursor_isInvalid( tmpl_csr ) ) {
    //
    // If it's not invalid, it means it's a function template specialization
    // which means it can't be static.
    //
    return false;
  }

  return true;
}

/**
 * Gets whether \a cursor is for a symbol definition.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor is for a symbol definition.
 *
 * @sa cursor_visit_def()
 */
NODISCARD
static bool cursor_is_definition( CXCursor cursor ) {
  if ( tidy_Cursor_isForwardDeclarable( cursor ) ) {
    //
    // These may be forward-declared, so ask whether this cursor is the
    // definition.
    //
    return tidy_isCursorDefinition( cursor );
  }

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_EnumConstantDecl:
    case CXCursor_FieldDecl:
    case CXCursor_LabelStmt:
    case CXCursor_MacroDefinition:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypedefDecl:
      return true;

    case CXCursor_EnumDecl:
    case CXCursor_Namespace:
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
      return !clang_Cursor_isAnonymous( cursor );

    case CXCursor_ParmDecl:
      //
      // Only parameters of function definitions count.  Parameters of function
      // declarations and pointer-to-function types would otherwise always look
      // unused.
      //
      if ( !tidy_Cursor_inFunctionScope( cursor ) )
        return false;
      CXCursor const fn_csr = clang_getCursorSemanticParent( cursor );
      if ( !tidy_isCursorDefinition( fn_csr ) )
        return false;
      if ( tidy_Cursor_isSpellingEqualTo( cursor, "" ) )
        return false;                   // skip unnamed parameters
      return true;

    default:
      return false;
  } // switch
}

/**
 * Gets whether \a cursor is nested, at any depth, within a class (or
 * enumeration) that is ignored via the `ignore-symbols` key.
 *
 * @param cursor The cursor to check.
 * @return Returns `true` only if \a cursor is within an ignored class (or
 * enumeration).
 */
NODISCARD
static bool cursor_in_ignored_class( CXCursor cursor ) {
  CXCursor scope_csr = clang_getCursorSemanticParent( cursor );

  while ( !tidy_Cursor_isInvalid( scope_csr ) ) {
    enum CXCursorKind const kind = clang_getCursorKind( scope_csr );
    if ( kind == CXCursor_TranslationUnit )
      break;

    if ( tidy_Cursor_isClassDecl( scope_csr ) || kind == CXCursor_EnumDecl ) {
      CXFile const  scope_file = tidy_getCursorLocation_File( scope_csr );
      char *const   scope_name = tidy_Cursor_getScopedSpelling( scope_csr );
      bool          is_ignored = false;

      if ( scope_file != NULL && scope_name != NULL && scope_name[0] != '\0' ) {
        CXString const    path_cxs = clang_getFileName( scope_file );
        char const *const path_cs = clang_getCString( path_cxs );
        is_ignored = config_symbol_is_ignored( scope_name, path_cs );
        clang_disposeString( path_cxs );
      }
      free( scope_name );

      if ( is_ignored )
        return true;
    }

    scope_csr = clang_getCursorSemanticParent( scope_csr );
  } // while

  return false;
}

/**
 * Gets whether \a sym_csr should be excluded from the global set.
 *
 * @param sym_csr The symbol's cursor to check.
 * @param source The tidy_source to use.
 * @return Returns `true` only if \a sym_csr is excluded.
 */
NODISCARD
static bool cursor_is_excluded( CXCursor sym_csr, tidy_source const *source ) {
  if ( tidy_Cursor_isInvalid( sym_csr ) )
    return true;                        // LCOV_EXCL_LINE

  CXFile const sym_file = tidy_getCursorLocation_File( sym_csr );
  if ( sym_file == NULL )
    return true;

  assert( source != NULL );

  if ( tidy_Cursor_inAnonymousClass( sym_csr ) )
    return true;

  //
  // We can't use clang_getCursorLanguage() because libclang doesn't initially
  // set what it thinks the current language is until it encounters something
  // language-specific.  So just use source->lang.
  //
  if ( tidy_Cursor_isReservedName( sym_csr, source->lang ) )
    return true;

  char *cls_name = NULL;
  bool  is_excluded = true;
  char *sym_scoped_name = tidy_Cursor_getScopedSpelling( sym_csr );

  if ( sym_scoped_name == NULL )
    goto done;

  if ( cursor_in_ignored_class( sym_csr ) )
    goto done;

  is_excluded = false;

  CXString const    sym_path_cxs = clang_getFileName( sym_file );
  char const *const sym_path_cs = clang_getCString( sym_path_cxs );
  bool const ignored = config_symbol_is_ignored( sym_scoped_name, sym_path_cs );
  clang_disposeString( sym_path_cxs );

  CXCursor const          parent_csr = clang_getCursorSemanticParent( sym_csr );
  enum CXCursorKind const sym_kind = clang_getCursorKind( sym_csr );

  switch ( sym_kind ) {
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
    case CXCursor_EnumConstantDecl:
    case CXCursor_EnumDecl:
    case CXCursor_FieldDecl:
    case CXCursor_FunctionDecl:
    case CXCursor_MacroDefinition:
    case CXCursor_ParmDecl:
    case CXCursor_StructDecl:
    case CXCursor_TypeAliasDecl:
    case CXCursor_TypedefDecl:
    case CXCursor_UnionDecl:
    case CXCursor_VarDecl:
      is_excluded = ignored;
      break;

    case CXCursor_Constructor:
      cls_name = tidy_Cursor_getScopedSpelling( parent_csr );
      if ( clang_CXXConstructor_isCopyConstructor( sym_csr ) &&
           config_bool_value( &opt_ignore_cxx_copy_ctors, cls_name ) ) {
        is_excluded = true;
      }
      if ( clang_CXXConstructor_isMoveConstructor( sym_csr ) &&
           config_bool_value( &opt_ignore_cxx_move_ctors, cls_name ) ) {
        is_excluded = true;
      }
      break;

    case CXCursor_CXXMethod:
      if ( ignored ) {
        is_excluded = true;
        break;
      }
      cls_name = tidy_Cursor_getScopedSpelling( parent_csr );
      if ( clang_CXXMethod_isCopyAssignmentOperator( sym_csr ) ) {
        if ( config_bool_value( &opt_ignore_cxx_copy_assigns, cls_name ) )
          is_excluded = true;
      }
      else if ( clang_CXXMethod_isMoveAssignmentOperator( sym_csr ) ) {
        if ( config_bool_value( &opt_ignore_cxx_move_assigns, cls_name ) )
          is_excluded = true;
      }
      break;

    case CXCursor_Namespace:
      //
      // Namespaces first declared in standard headers (e.g., "std") should be
      // excluded even if re-opened in user code.
      //
      sym_csr = clang_getCanonicalCursor( sym_csr );
      return tidy_Cursor_isInSystemHeader( sym_csr );

    default:
      /* suppress warning */;
  } // switch

done:
  free( cls_name );
  free( sym_scoped_name );
  return is_excluded;
}

/**
 * Gets whether \a csr is directly listed as a root symbol, i.e., via the config
 * file's `root-symbols`, `root-symbols-from`, or per-file `root-symbols` keys.
 *
 * @param csr The (canonical) cursor to check.
 * @param name The scoped name of \a csr.
 * @return Returns `true` only if \a csr is listed as a root.
 *
 * @sa cursor_is_config_root()
 */
NODISCARD
static bool cursor_is_listed_root( CXCursor csr, char const *name ) {
  assert( name != NULL );

  CXFile const file = tidy_getCursorLocation_File( csr );
  if ( file == NULL )
    return false;                       // LCOV_EXCL_LINE

  CXString const    path_cxs = clang_getFileName( file );
  char const *const path_cs = clang_getCString( path_cxs );
  bool const        is_root = config_symbol_is_root( name, path_cs );

  clang_disposeString( path_cxs );
  return is_root;
}

/**
 * Gets whether \a sym_csr is configured to be a root symbol, either:
 *
 *  + Directly, via cursor_is_listed_root(); or:
 *  + Being a C++ member function of a class that's a root so the member
 *    functions of a root class needn't be listed individually.
 *
 * @remarks Being configured as a root isn't sufficient to be treated as one.
 *
 * @param sym_csr The symbol's (canonical) cursor to check.
 * @param sym_name The scoped name of the symbol.
 * @return Returns `true` only if \a sym_csr is configured to be a root.
 */
NODISCARD
static bool cursor_is_config_root( CXCursor sym_csr, char const *sym_name ) {
  assert( sym_name != NULL );

  if ( cursor_is_listed_root( sym_csr, sym_name ) )
    return true;

  switch ( clang_getCursorKind( sym_csr ) ) {
    case CXCursor_Constructor:
    case CXCursor_ConversionFunction:
    case CXCursor_CXXMethod:
    case CXCursor_Destructor:
    case CXCursor_FunctionTemplate:     // member function template
      break;
    default:
      return false;
  } // switch

  //
  // The semantic parent (rather than the lexical one) is the class even for a
  // member function defined outside of it, e.g.: `void C::f() { }`.
  //
  CXCursor const cls_csr =
    clang_getCanonicalCursor( clang_getCursorSemanticParent( sym_csr ) );
  switch ( clang_getCursorKind( cls_csr ) ) {
    case CXCursor_ClassDecl:
    case CXCursor_ClassTemplate:
    case CXCursor_ClassTemplatePartialSpecialization:
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
      break;
    default:
      return false;
  } // switch

  char *const cls_name = tidy_Cursor_getScopedSpelling( cls_csr );
  if ( cls_name == NULL )
    return false;                       // LCOV_EXCL_LINE
  bool const is_root = cursor_is_listed_root( cls_csr, cls_name );
  free( cls_name );
  return is_root;
}

/**
 * Dispatch to the visitor-handling function based on the kind of \a sym_csr.
 *
 * @note This function must only be called for a cursor that _is_ a definition.
 *
 * @param sym_csr The cursor for a definition.
 * @param sym The tidy_symbol for \a sym_csr.
 * @param svd The symbols_visit_data to use.
 *
 * @sa cursor_is_definition()
 * @sa cursor_visit_ref()
 */
static void cursor_visit_def( CXCursor sym_csr, tidy_symbol *sym,
                              symbols_visit_data *svd ) {
  assert( sym != NULL );
  assert( svd != NULL );

  if ( tidy_Cursor_hasMaybeUnusedAttr( sym_csr ) )
    sym->is_dead = true;

  enum CXCursorKind const kind = clang_getCursorKind( sym_csr );

  switch ( kind ) {
    case CXCursor_LabelStmt:
      //
      // Never set curr_sym to a labeled statement since it can break a
      // ref_sym_set.  Consider:
      //
      //      void g() { }
      //
      //      void f() {
      //        unused_label: g();
      //      }
      //
      //      int main() {
      //        f();
      //      }
      //
      // If we set curr_sym to unused_label here, we'd get the following
      // symbols and what they reference:
      //
      //      f() -> {}
      //      g() -> {}
      //      main() [root] -> { f() }
      //      unused_label -> { g() }
      //
      // Note that f() does _not_ reference unused_label.  (If it did, then
      // ununsed label would incorrectly be considered used.  A actual goto
      // statement is needed to reference it.)
      //
      // Since nothing references unused_label, it's correctly left as unused.
      // But unused_label is the only symbol that has a reference to g(), so
      // g() would be left as unused also --- incorrectly.
      //
      // By _not_ setting curr_sym to unused_label, we'd get the following
      // symbols and what they reference:
      //
      //      f() -> { g() }
      //      g() -> {}
      //      main() [root] -> { f() }
      //      unused_label -> {}
      //
      // Now, f() references g(), so it would be marked used --- correctly.
      //
      break;

    case CXCursor_ParmDecl:
      //
      // Never set curr_sym to a parameter since it can break a ref_sym_set.
      // Consider:
      //
      //      constexpr int N = 42;
      //
      //      void f( int unused_param = N ) {
      //      }
      //
      //      int main() {
      //        f();
      //      }
      //
      // The default argument is a child of the parameter's declaration.  If we
      // set curr_sym to unused_param here, we'd get the following symbols and
      // what they reference:
      //
      //      f() -> {}
      //      N -> {}
      //      main() [root] -> { f() }
      //      unused_param -> { N }
      //
      // Note that f() does _not_ reference unused_param.  (If it did, then
      // unused_param would incorrectly be considered used.  An actual use of
      // the parameter in f()'s body is needed to reference it.)
      //
      // Since nothing references unused_param, it's correctly left as unused.
      // But unused_param is the only symbol that has a reference to N, so N
      // would be left as unused also --- incorrectly.  The default argument is
      // evaluated whenever f() is called without that argument.
      //
      // By _not_ setting curr_sym to unused_param, we'd get the following
      // symbols and what they reference:
      //
      //      f() -> { N }
      //      N -> {}
      //      main() [root] -> { f() }
      //      unused_param -> {}
      //
      // Now, f() references N, so it would be marked used --- correctly.
      //
      break;

    default:
      svd->curr_sym = sym;
  } // switch

  switch ( kind ) {
    case CXCursor_Constructor:
      visit_Constructor( sym_csr, svd );
      break;

    case CXCursor_CXXMethod:
    case CXCursor_Destructor:
      visit_CXXMethod( sym_csr, svd );
      break;

    case CXCursor_FieldDecl:
      visit_FieldDecl( sym_csr, svd );
      break;

    case CXCursor_FunctionDecl:
      visit_FunctionDecl( sym_csr, svd );
      break;

    case CXCursor_MacroDefinition:
      visit_MacroDefinition( sym_csr, svd );
      break;

    case CXCursor_VarDecl:
      visit_VarDecl( sym_csr, svd );
      break;

    default:
      /* suppress warning */;
  } // switch
}

/**
 * A helper function for symbols_visitor() that dispatches to the visitor-
 * handling function for reference kinds of cursors.
 *
 * @note This function most only be called for a cursor that is _not_ a
 * definition.
 *
 * @param cursor The cursor being visited.
 * @param svd The symbols_visit_data to use.
 *
 * @sa cursor_visit_def()
 */
static void cursor_visit_ref( CXCursor cursor, symbols_visit_data *svd ) {
  assert( svd != NULL );

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  switch ( kind ) {
    case CXCursor_CallExpr:
    case CXCursor_CXXFunctionalCastExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXConstCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
    case CXCursor_CXXDynamicCastExpr:
    case CXCursor_DeclRefExpr:
    case CXCursor_LabelRef:
    case CXCursor_MacroExpansion:
    case CXCursor_MemberRefExpr:
    case CXCursor_TemplateRef:
    case CXCursor_TypeRef:
      visit_most_Ref( cursor, svd );
      break;

    case CXCursor_CXXDeleteExpr:
      visit_CXXDeleteExpr( cursor, svd );
      break;

    case CXCursor_CXXForRangeStmt:
      visit_CXXForRangeStmt( cursor, svd );
      break;

    case CXCursor_CXXNewExpr:
      visit_CXXNewExpr( cursor, svd );
      break;

    case CXCursor_FunctionDecl:
      if ( !svd->ignore_forwards )
        fwd_dec_maybe_unneeded( cursor, svd );
      break;

    case CXCursor_OverloadedDeclRef:
      visit_OverloadedDeclRef( cursor, svd );
      break;

    default:
      /* suppress warning */;
  } // switch
}

/**
 * Calls `clang_getFileName()` on \a file and returns a copy of the path, but
 * only if it's a source file.
 *
 * @param file The file to get the path of.
 * @return Returns a copy of the path or NULL if \a file isn't a source path.
 *
 * @note The caller is responsible for freeing the returned string.
 *
 * @sa is_source_path()
 */
NODISCARD
static char* dup_if_source_path( CXFile file ) {
  CXString const    path_cxs = clang_getFileName( file );
  char const *const path_cs = clang_getCString( path_cxs );
  char *const       rv_path = is_source_path( path_cs ) ?
    strdup_or_exit( path_no_dot_slash( path_cs ) ) : NULL;
  clang_disposeString( path_cxs );
  return rv_path;
}

/**
 * Gets whether the file for a top-level cursor can be skipped because it was
 * already visted for an earlier translation unit of the same language.
 *
 * @remarks The first top-level cursor from a file registers it with the
 * current source's serial and is visited, as are the rest of that file's
 * cursors in the same source.  In later sources, the serial differs, so
 * they're skipped.
 *
 * @param file The cursor's file.
 * @param svd The symbols_visit_data to use.
 * @return Returns `true` only if the file was already seen.
 */
NODISCARD
static bool file_previously_seen( CXFile file, symbols_visit_data *svd ) {
  assert( svd != NULL );

  if ( file == NULL || clang_File_isEqual( file, svd->source->file ) )
    return false;                       // never skip the TU's own source

  //
  // Consecutive top-level cursors are usually from the same file: if so,
  // return the last result.
  //
  if ( clang_File_isEqual( file, svd->fps.last_file ) )
    return svd->fps.last_seen;

  bool seen = false;

  tidy_header find_header = {
    .fuid = tidy_getFileUniqueID( file ),
    .lang = svd->source->lang,
    .tsid = svd->source->tsid
  };
  ht_insert_rv_t const hti =
    ht_table_insert( &header_set, &find_header, sizeof find_header );
  if ( !hti.inserted ) {
    tidy_header const *const found_header = HT_DINT( hti.entry );
    seen = found_header->tsid != svd->source->tsid;
    if ( seen )
      ++header_stats.skipped_count;
    else
      ++header_stats.revisit_count;
  }

  svd->fps.last_file = file;
  svd->fps.last_seen = seen;
  return seen;
}

/**
 * Cleans-up a tidy_loc.
 *
 * @param tloc The tidy_loc to clean up.  If NULL, does nothing.
 */
static void loc_cleanup( tidy_loc *tloc ) {
  if ( tloc != NULL )
    FREE( tloc->path );
}

/**
 * Compares two \ref tidy_loc objects by their path, then by line, then by
 * column.
 *
 * @param i_loc The first location.
 * @param j_loc The second location.
 * @return Returns a number less than 0, 0, or greater than 0 if the first
 * location is less than, equal to, or greater than the second, respectively.
 */
NODISCARD
static int loc_cmp( tidy_loc const *i_loc, tidy_loc const *j_loc ) {
  assert( i_loc != NULL );
  assert( i_loc->path != NULL );
  assert( j_loc != NULL );
  assert( j_loc->path != NULL );

  int const cmp = strcmp( i_loc->path, j_loc->path );
  if ( cmp != 0 )
    return cmp;
  if ( i_loc->line < j_loc->line ) return -1;
  if ( i_loc->line > j_loc->line ) return  1;
  // LCOV_EXCL_START
  if ( i_loc->col  < j_loc->col  ) return -1;
  if ( i_loc->col  > j_loc->col  ) return  1;
  return 0;
  // LCOV_EXCL_STOP
}

/**
 * Gets a tidy_loc for the location of a declaration, but only if it's in a
 * source path.
 *
 * @param sloc The source location of a declaration.
 * @return Returns a tidy_loc (spelling location) for \a sloc or one having
 * a NULL \ref tidy_loc::path "path" if \a sloc isn't in a file or the file
 * isn't a source path.
 *
 * @note The caller is responsible for calling loc_cleanup().
 */
NODISCARD
static tidy_loc loc_from_SourceLocation( CXSourceLocation sloc ) {
  CXFile    file;
  unsigned  line, col;
  tidy_loc  rv_loc = { 0 };

  clang_getSpellingLocation( sloc, &file, &line, &col, /*offset=*/NULL );
  if ( file != NULL ) {
    rv_loc.path = dup_if_source_path( file );
    if ( rv_loc.path != NULL ) {
      rv_loc.line = line;
      rv_loc.col  = col;
    }
  }

  return rv_loc;
}

/**
 * Prints \a tloc as
 * <tt>"</tt><i>path</i><tt>":</tt><i>line</i><tt>,</tt><i>col</i>.
 *
 * @note The path is printed only if it's non-NULL.
 *
 * @param tloc The tidy_loc to print.
 */
static void loc_print( tidy_loc const *tloc ) {
  assert( tloc != NULL );

  if ( tloc->path != NULL ) {
    color_start( stderr, sgr_locus );
    EPRINTF( "\"%s\":", tloc->path );
    color_end( stderr, sgr_locus );
  }
  color_start( stderr, sgr_locus );
  EPRINTF( "%u", tloc->line );
  color_end( stderr, sgr_locus );
  EPUTC( ',' );
  color_start( stderr, sgr_locus );
  EPRINTF( "%u", tloc->col );
  color_end( stderr, sgr_locus );
}

/**
 * For a macro, gets the cursor for the identifier given by \a token within \a
 * scope_csr, but only if \a token actually is an identifier, neither
 * `__VA_ARGS__` nor `__VA_OPT__`, nor one of the current macro's parameters.
 *
 * @remarks This is a variant of tidy_getCursorByNameToken(), but for a macro
 * that additionally takes \a param_set.
 *
 * @param tu The translation unit to use.
 * @param token The token to get the cursor for.
 * @param scope_csr The cursor of the scope to search within.
 * @param param_set The set of macro parameter names.
 * @return Returns said cursor; or an invalid cursor if \a token is an
 * identifier, but not found; or the null cursor if \a token is:
 *  + Not an identifier; or:
 *  + Either `__VA_ARGS__` nor `__VA_OPT__`; or:
 *  + In \a param_set.
 *
 * @sa tidy_getCursorByNameToken()
 */
NODISCARD
static CXCursor macro_getCursorByNameToken( CXTranslationUnit tu,
                                            CXToken token, CXCursor scope_csr,
                                            hash_table_t const *param_set ) {
  assert( param_set != NULL );

  if ( clang_getTokenKind( token ) != CXToken_Identifier )
    return clang_getNullCursor();

  CXString const    token_cxs = clang_getTokenSpelling( tu, token );
  char const *const token_cs = clang_getCString( token_cxs );

  CXCursor const rv_csr =
    strcmp( token_cs, "__VA_ARGS__" ) != 0 &&
    strcmp( token_cs, "__VA_OPT__" ) != 0 &&
    ht_table_find( param_set, token_cs ) == NULL ?
      tidy_getCursorByName( token_cs, scope_csr )
    :
      clang_getNullCursor();

  clang_disposeString( token_cxs );
  return rv_csr;
}

/**
 * Gets the names of all of a macro's parameters.
 *
 * @param tu The translation unit to use.
 * @param tokens The array of macro tokens.
 * @param token_count The length of \a tokens.
 * @param param_set The set to add the parameter names to.
 * @return Returns the index of the token one past the `)`.
 */
NODISCARD
static unsigned macro_get_params( CXTranslationUnit tu,
                                  CXToken const tokens[static 2],
                                  unsigned token_count,
                                  hash_table_t *param_set ) {
  assert( param_set != NULL );

  if ( unlikely( token_count < 2 ) )
    return 1;                           // LCOV_EXCL_LINE

  unsigned rv_idx = 1;

  // Start at index 2 since tokens[0] is the macro name, tokens[1] is the '('.
  for ( unsigned i = 2; i < token_count; ++i ) {
    CXTokenKind const kind = clang_getTokenKind( tokens[i] );
    switch ( kind ) {
      case CXToken_Identifier:
      case CXToken_Punctuation:
        break;
      default:
        continue;
    } // switch

    CXString const    token_cxs = clang_getTokenSpelling( tu, tokens[i] );
    char const *const token_cs = clang_getCString( token_cxs );

    switch ( kind ) {
      case CXToken_Identifier:
        ht_table_insert(
          param_set, CONST_CAST( char*, token_cs ), strlen( token_cs ) + 1/*\0*/
        );
        break;
      case CXToken_Punctuation:
        if ( strcmp( token_cs, ")" ) == 0 ) {
          rv_idx = i + 1;
          i = token_count;              // will cause loop to exit
        }
        break;
      default:
        unreachable();
    } // switch

    clang_disposeString( token_cxs );
  } // for

  return rv_idx;
}

/**
 * For a macro, gets the cursor for the scoped symbol from \a tokens.
 *
 * @remarks This is a variant of tidy_Token_getScopedNameCursor(), but for a
 * macro that additionally takes \a param_set.
 *
 * @param tu The translation unit to use.
 * @param tokens The array of macro tokens.
 * @param token_count The length of \a tokens.
 * @param ptoken_idx A pointer to the current index within \a tokens.
 * @param param_set The set of macro parameter names.
 * @param lang The language of the source file being tidied.
 * @return Returns said cursor or the null cursor for none.
 *
 * @sa tidy_Token_getScopedNameCursor()
 */
NODISCARD
static
CXCursor macro_Token_getScopedNameCursor( CXTranslationUnit tu,
                                          CXToken const tokens[],
                                          unsigned token_count,
                                          unsigned *ptoken_idx,
                                          hash_table_t const *param_set,
                                          enum CXLanguageKind lang ) {
  assert( param_set != NULL );
  unsigned token_idx = *ptoken_idx;

  CXCursor const tu_csr = clang_getTranslationUnitCursor( tu );
  CXCursor rv_csr =
    macro_getCursorByNameToken( tu, tokens[ *ptoken_idx ], tu_csr, param_set );

  if ( lang == CXLanguage_CPlusPlus ) {
    while ( !tidy_Cursor_isInvalid( rv_csr ) ) {
      unsigned next_idx = token_idx;
      CXToken const *t;

      // Look for "::".
      if ( (t = tidy_Token_getNext( tokens, token_count, &next_idx )) == NULL )
        break;
      if ( !tidy_Token_isScopeQualifier( tu, *t ) )
        break;

      // Look for an identifier.
      if ( (t = tidy_Token_getNext( tokens, token_count, &next_idx )) == NULL )
        break;
      CXCursor const next_csr =
        macro_getCursorByNameToken( tu, *t, rv_csr, param_set );
      if ( tidy_Cursor_isInvalid( next_csr ) )
        break;                          // LCOV_EXCL_LINE

      rv_csr = next_csr;
      token_idx = next_idx;
    } // while

    *ptoken_idx = token_idx;
  }

  return rv_csr;
}

// LCOV_EXCL_START
/**
 * Prints statistics for a hash table that has the maximum load factor among
 * all symbols.
 *
 * @param vsc The verbose_stat_ctx to use.
 * @param mbr_off The offset of the hash table (from `offsetof`).
 * @param abbr The abbreviation to use.
 */
static void print_max_lf_stats( verbose_stat_ctx *vsc, size_t mbr_off,
                                char const *abbr ) {
  tidy_symbol const *max_sym = NULL;
  double max_lf = -1.0;

  ht_iterator_t iter;
  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_dead )
      continue;
    hash_table_t const *const set = PTR_TO_MBR( sym, mbr_off );
    if ( set->size > 0 ) {
      double const lf = ht_table_load_factor( set );
      if ( lf > max_lf ) {
        max_sym = sym;
        max_lf = lf;
      }
    }
  } // for

  if ( max_sym == NULL )
    return;

  ++vsc->indent;
  verbose_statf( vsc, "%s-max-load-factor:\n", abbr );
  verbose_statf( vsc,
    "%s-name = \"%s\"\n", abbr, symbol_name_for_print( max_sym )
  );
  if ( max_sym->def_loc.path != NULL )
    verbose_statf( vsc, "%s-path = \"%s\"\n", abbr, max_sym->def_loc.path );
  verbose_statf( vsc, "%s-load-factor = " TIDY_STAT_LF_FMT "\n", abbr, max_lf );
  verbose_statf( vsc, "%s-size = %u\n", abbr, max_sym->ref_sym_set.size );
  --vsc->indent;
}

/**
 * Prints statistics for symbols if requested.
 *
 * @param vsc The verbose_stat_ctx to use.
 */
static void print_statistics( verbose_stat_ctx *vsc ) {
  verbose_statf( vsc, "header set:\n" );
  verbose_statf( vsc,
    "hs-load-factor = " TIDY_STAT_LF_FMT "\n",
    ht_table_load_factor( &header_set )
  );
  verbose_statf( vsc, "hs-size = %u\n", header_set.size );
  verbose_statf( vsc, "hs-revisit-count = %u\n", header_stats.revisit_count );
  verbose_statf( vsc, "hs-skipped-count = %'u\n", header_stats.skipped_count );

  verbose_statf( vsc, "symbol set:\n" );
  verbose_statf( vsc,
    "ss-load-factor = " TIDY_STAT_LF_FMT "\n",
    ht_table_load_factor( &tidy_symbol_set )
  );
  verbose_statf( vsc, "ss-size = %u\n", tidy_symbol_set.size );

  print_max_lf_stats( vsc, offsetof( tidy_symbol, ref_sym_set ), "ss-rss" );
  print_max_lf_stats( vsc,
    offsetof( tidy_symbol, cxx_adl_usr_set ), "ss-adl-usr"
  );
}
// LCOV_EXCL_STOP

/**
 * Add the symbol's path, line, and column to \a sym.
 *
 * @param sym The tidy_symbol to add the definition for.
 * @param sym_loc The source location of the cursor for \a sym.
 * @param svd The symbols_visit_data to use.
 */
static void symbol_add_def( tidy_symbol *sym, CXSourceLocation sym_loc,
                            symbols_visit_data const *svd ) {
  assert( sym != NULL );

  CXFile ref_file, def_file;
  unsigned def_line, def_col;

  clang_getExpansionLocation(
    sym_loc, &ref_file, &sym->def_loc.line, &sym->def_loc.col, /*offset=*/NULL
  );
  if ( unlikely( ref_file == NULL ) )
    return;                             // LCOV_EXCL_LINE

  clang_getSpellingLocation(
    sym_loc, &def_file, &def_line, &def_col, /*offset=*/NULL
  );
  if ( unlikely( def_file == NULL ) )
    return;                             // LCOV_EXCL_LINE

  char *ref_path = dup_if_source_path( ref_file );
  if ( unlikely( ref_path == NULL ) )
    return;                             // LCOV_EXCL_LINE

  bool const is_defined_in_macro =
    !clang_File_isEqual( def_file, ref_file ) ||
    def_line != sym->def_loc.line ||
    def_col  != sym->def_loc.col;

  char *macro_path = NULL;

  if ( is_defined_in_macro && !clang_File_isEqual( def_file, ref_file ) ) {
    macro_path = dup_if_source_path( def_file );
    if ( macro_path == NULL )           // e.g., defined in a system header
      goto skip;                        // LCOV_EXCL_LINE
  }

  FREE( sym->def_loc.path );            // just in case
  sym->def_loc.path = str_move( &ref_path );

  if ( is_defined_in_macro ) {
    loc_cleanup( &sym->macro.spelling_loc );
    sym->macro.spelling_loc = (tidy_loc){
      .path = macro_path,
      .line = def_line,
      .col  = def_col
    };
    symbol_update_macro_name_loc( sym, ref_file, svd );
  }

skip:
  free( ref_path );
}

/**
 * Cleans-up a tidy_symbol.
 *
 * @param sym The tidy_symbol to clean up.  If NULL, does nothing.
 */
static void symbol_cleanup( tidy_symbol *sym ) {
  if ( sym != NULL ) {
    FREE( sym->key );
    FREE( sym->name );
    FREE( sym->cxx_prototype );
    loc_cleanup( &sym->dec_loc );
    loc_cleanup( &sym->def_loc );
    loc_cleanup( &sym->ext_link.header_dec_loc );

    loc_cleanup( &sym->macro.spelling_loc );
    loc_cleanup( &sym->macro.name_loc );
    FREE( sym->macro.name );

    ht_table_cleanup( &sym->ref_sym_set, /*free_fn=*/NULL );
    ht_table_cleanup( &sym->cxx_adl_usr_set, /*free_fn=*/NULL );
  }
}

/**
 * Compares two \ref tidy_symbol objects.
 *
 * @param i_sym The first symbol.
 * @param j_sym The second symbol.
 * @return Returns a number less than 0, 0, or greater than 0 if the name of \a
 * i_sym is less than, equal to, or greater than the name of \a j_sym,
 * respectively.
 *
 * @sa symbol_cmp_by_dec_loc()
 * @sa symbol_cmp_by_def_loc()
 */
NODISCARD
static int symbol_cmp( tidy_symbol const *i_sym, tidy_symbol const *j_sym ) {
  assert( i_sym != NULL );
  assert( j_sym != NULL );
  return strcmp( i_sym->key, j_sym->key );
}

/**
 * Compares two \ref tidy_symbol objects by their declaration location.
 *
 * @param i_sym The first symbol.
 * @param j_sym The second symbol.
 * @return Returns a number less than 0, 0, or greater than 0 if the first
 * symbol's location is less than, equal to, or greater than the second
 * symbol's location, respectively.
 *
 * @sa symbol_cmp()
 * @sa symbol_cmp_by_def_loc()
 */
NODISCARD
static int symbol_cmp_by_dec_loc( tidy_symbol const *i_sym,
                                  tidy_symbol const *j_sym ) {
  assert( i_sym != NULL );
  assert( j_sym != NULL );
  return loc_cmp( &i_sym->dec_loc, &j_sym->dec_loc );
}

/**
 * Compares two \ref tidy_symbol objects by their definition location.
 *
 * @param i_sym The first symbol.
 * @param j_sym The second symbol.
 * @return Returns a number less than 0, 0, or greater than 0 if the first
 * symbol's location is less than, equal to, or greater than the second
 * symbol's location, respectively.
 *
 * @sa symbol_cmp()
 * @sa symbol_cmp_by_dec_loc()
 */
NODISCARD
static int symbol_cmp_by_def_loc( tidy_symbol const *i_sym,
                                  tidy_symbol const *j_sym ) {
  assert( i_sym != NULL );
  assert( j_sym != NULL );
  return loc_cmp( &i_sym->def_loc, &j_sym->def_loc );
}

/**
 * Calculates the hash of \a sym.
 *
 * @param sym The tidy_symbol to calculate the hash for.
 * @return Returns said hash.
 */
NODISCARD
static ht_hash_val_t symbol_hash( tidy_symbol const *sym ) {
  return fnv1a_s( sym->key );
}

/**
 * Gets whether \a sym is within a scope that is ununsed.
 *
 * @param sym The symbol to check.
 * @return Returns `true` only if \a sym is within an unused scope.
 */
NODISCARD
static bool symbol_in_unused_scope( tidy_symbol const *sym ) {
  assert( sym != NULL );

  for ( sym = sym->parent_sym; sym != NULL; sym = sym->parent_sym ) {
    if ( sym->is_scope && !sym->is_used )
      return true;
  } // for

  return false;
}

/**
 * Gets whether \a sym has external linkage unnecessarily.
 *
 * @remarks
 * @parblock
 * A symbol has external linkage unnecessarily if it:
 *
 *  + Has external linkage; and:
 *  + Is defined and used only in a single source file.
 *
 * @endparblock
 *
 * @param sym The symbol to check.
 * @return Returns `true` only if \a sym has external linkage unnecessarily.
 *
 * @note This must be called only after symbols_mark_used().
 */
NODISCARD
static bool symbol_is_unneeded_extern( tidy_symbol const *sym ) {
  assert( sym != NULL );

  // These checks can be in any order.
  if ( !sym->ext_link.is_maybe_internal )
    return false;                       // not a candidate for internal
  if ( sym->is_dead )
    return false;                       // dead symbols are ignored
  if ( sym->is_root )
    return false;                       // roots must have external linkage
  if ( !sym->is_used )
    return false;                       // unused are reported separately
  if ( sym->ext_link.is_ref_from_other_file )
    return false;                       // referenced from another file

  // But this check must come after those.
  if ( sym->ext_link.ref_tsid == 0 )
    return true;                        // used, but referenced indirectly

  // And this check must come after the one above.
  if ( sym->ext_link.ref_tsid != sym->ext_link.def_tsid )
    return false;                       // referenced from another source

  return true;                          // referenced only from defining source
}

/**
 * Marks a symbol as used.
 *
 * @param sym The symbol to mark as used.
 */
static void symbol_mark_used( tidy_symbol *sym ) {
  assert( sym != NULL );

  if ( !sym->is_used ) {
    sym->is_used = true;
    if ( sym->parent_sym != NULL )
      symbol_mark_used( sym->parent_sym );
    ht_iterator_t iter;
    ht_iterator_init( &iter, &sym->ref_sym_set );
    for ( tidy_symbol *ref_sym;
          (ref_sym = ht_iterator_next( &iter )) != NULL; ) {
      symbol_mark_used( ref_sym );
    } // for
  }
}

/**
 * Prints a tidy_symbol.
 *
 * @param sym The tidy_symbol to print.
 * @param mbr_off The offset of the tidy_loc member of \a sym whose location to
 * print, e.g., `def_loc` or `dec_loc` (from `offsetof`).
 */
static void symbol_print( tidy_symbol const *sym, size_t mbr_off ) {
  assert( sym != NULL );

  bool const is_def_in_macro = symbol_is_def_in_macro( sym );
  tidy_loc const *const tloc = PTR_TO_MBR( sym, mbr_off );

  color_start( stderr, sgr_locus );
  EPRINTF( "%5u", tloc->line );
  color_end( stderr, sgr_locus );
  if ( is_def_in_macro ) {
    EPUTS( "   " );                     // same width as ",%2u"
  }
  else {
    EPUTC( ',' );
    color_start( stderr, sgr_locus );
    EPRINTF( "%2u", tloc->col );
    color_end( stderr, sgr_locus );
  }

  EPRINTF( " | \"%s\"", symbol_name_for_print( sym ) );
  if ( is_def_in_macro ) {
    EPUTS( " (defined at " );
    loc_print( &sym->macro.spelling_loc );
    if ( sym->macro.name_loc.line > 0 ) {
      EPRINTF( " via macro \"%s\" at ", sym->macro.name );
      loc_print( &sym->macro.name_loc );
    }
    EPUTC( ')' );
  }
}

/**
 * Prints a path of \a sym.
 *
 * @param sym The tidy_symbol to print the path of.
 * @param mbr_off The offset of the tidy_loc member of \a sym whose path to
 * print, e.g., `def_loc` or `dec_loc` (from `offsetof`).
 */
static void symbol_print_path( tidy_symbol const *sym, size_t mbr_off ) {
  assert( sym != NULL );

  tidy_loc const *const tloc = PTR_TO_MBR( sym, mbr_off );

  color_start( stderr, sgr_locus );
  EPRINTF( "\"%s\"", tloc->path );
  color_end( stderr, sgr_locus );
  EPUTS( ": " );
  color_start( stderr, sgr_warning );
  EPUTS( "warning" );
  color_end( stderr, sgr_warning );
}

/**
 * Checks \a sym (that \a sym_csr defines) to see if it has external linkage
 * and is a candidate for having internal linkage instead, and, if so, where
 * its first declaration is when it's in a header.
 *
 * @param sym The tidy_symbol for \a sym_csr.
 * @param sym_csr The cursor for the definition of \a sym.
 * @param sym_loc The source location of \a sym_csr.
 * @param svd The symbols_visit_data to use.
 */
static void symbol_update_dec_loc( tidy_symbol *sym, CXCursor sym_csr,
                                   CXSourceLocation sym_loc,
                                   symbols_visit_data const *svd ) {
  assert( sym != NULL );
  assert( svd != NULL );

  if ( svd->ignore_extern ) {
    //
    // Checking is disabled for this source, so sym can't be a candidate.  A
    // reference from this source to a symbol defined elsewhere is still
    // counted as a reference from another file by symbol_update_ref_origin().
    //
    return;
  }

  sym->ext_link.def_tsid = svd->source->tsid;
  sym->ext_link.is_def_in_main_file =
    tidy_Location_isFromFile( sym_loc, svd->source->file );

  if ( !sym->ext_link.is_def_in_main_file ||
       !cursor_is_maybe_internal( sym_csr ) ) {
    return;
  }
  sym->ext_link.is_maybe_internal = true;

  //
  // The canonical cursor is the first declaration.  If it's in a header, then
  // that declaration has to be deleted or moved if the symbol is made static.
  //
  CXCursor const canon_csr = clang_getCanonicalCursor( sym_csr );
  if ( clang_equalCursors( canon_csr, sym_csr ) )
    return;
  CXSourceLocation const canon_loc = clang_getCursorLocation( canon_csr );
  if ( tidy_Location_isFromFile( canon_loc, svd->source->file ) )
    return;

  tidy_loc const header_dec_loc = loc_from_SourceLocation( canon_loc );
  if ( header_dec_loc.path != NULL ) {
    loc_cleanup( &sym->ext_link.header_dec_loc );
    sym->ext_link.header_dec_loc = header_dec_loc;
  }
}

/**
 * Updates \a sym's \ref tidy_symbol::ts_macro::name_loc "macro.name_loc" to
 * the location of the name of the macro whose expansion defined \a sym, i.e.,
 * the macro's name in its `#define`.
 *
 * @note If the macro in the same file as \a sym's definition, the \ref
 * tidy_loc::path "path" of \a sym's \ref tidy_symbol::ts_macro::name_loc
 * "macro.name_loc" is set to NULL.
 *
 * @param sym The tidy_symbol to update the macro defintion location of.
 * @param sym_file The file \a sym's expansion location is in.
 * @param svd The symbols_visit_data to use.
 */
static void symbol_update_macro_name_loc( tidy_symbol *sym, CXFile sym_file,
                                          symbols_visit_data const *svd ) {
  assert( sym != NULL );
  assert( svd != NULL );

  CXSourceLocation const sym_exp_loc = clang_getLocation(
    svd->source->tu, sym_file, sym->def_loc.line, sym->def_loc.col
  );
  if ( clang_equalLocations( sym_exp_loc, clang_getNullLocation() ) )
    return;

  CXCursor const macro_exp_csr =
    clang_getCursor( svd->source->tu, sym_exp_loc );
  enum CXCursorKind const kind = clang_getCursorKind( macro_exp_csr );
  if ( kind != CXCursor_MacroExpansion )
    return;

  CXCursor const macro_def_csr = clang_getCursorReferenced( macro_exp_csr );
  if ( tidy_Cursor_isInvalid( macro_def_csr ) )
    return;

  CXSourceLocation const macro_name_loc =
    clang_getCursorLocation( macro_def_csr );

  CXFile macro_def_file;
  unsigned macro_def_line, macro_def_col;
  clang_getSpellingLocation(
    macro_name_loc, &macro_def_file, &macro_def_line, &macro_def_col,
    /*offset=*/NULL
  );
  if ( macro_def_file == NULL )
    return;

  char *macro_def_path = NULL;          // NULL means same file as sym
  if ( !clang_File_isEqual( macro_def_file, sym_file ) ) {
    macro_def_path = dup_if_source_path( macro_def_file );
    if ( macro_def_path == NULL )       // e.g., defined in a system header
      return;
  }

  char *const macro_name = tidy_dupCursorSpelling( macro_def_csr );
  if ( likely( macro_name != NULL ) ) {
    FREE( sym->macro.name );
    sym->macro.name = macro_name;
    loc_cleanup( &sym->macro.name_loc );
    sym->macro.name_loc = (tidy_loc){
      .path = str_move( &macro_def_path ),
      .line = macro_def_line,
      .col  = macro_def_col
    };
  }

  free( macro_def_path );
}

/**
 * Updates \a to_sym's reference-origin information given that it's referenced
 * by \a from_sym (or from file scope if NULL) in the current translation unit.
 *
 * @remarks A reference from anything not defined in the current source's main
 * file (e.g., a header) counts as a reference from "elsewhere" since the
 * header may be included by other translation units.
 *
 * @param from_sym The referencing symbol.  If NULL, it means from file scope.
 * @param to_sym The referenced symbol.
 * @param svd The symbols_visit_data to use.
 */
static void symbol_update_ref_origin( tidy_symbol const *from_sym,
                                      tidy_symbol *to_sym,
                                      symbols_visit_data const *svd ) {
  assert( to_sym != NULL );
  assert( svd != NULL );

  if ( to_sym->ext_link.is_ref_from_other_file )
    return;

  if (  //
        // The referencing symbol is ...
        //
        (from_sym != NULL && (
          //
          // ... not defined in a source's main file which means it's in a
          // header file that other source files may include; or in a source
          // file for which checking is ignored (so this flag was never set).
          //
          !from_sym->ext_link.is_def_in_main_file ||

          //
          // ... defined in a source file other than the one being visited.
          // This shouldn't happen: it's a defensive check.
          //
          unlikely( from_sym->ext_link.def_tsid != svd->source->tsid )
        )) ||

        //
        // An earlier reference came from a source file other than the current
        // one.
        //
        (to_sym->ext_link.ref_tsid != 0 &&
         to_sym->ext_link.ref_tsid != svd->source->tsid) ) {
    to_sym->ext_link.is_ref_from_other_file = true;
  }
  else {
    //
    // The first reference (or another one from the same source's main file).
    //
    to_sym->ext_link.ref_tsid = svd->source->tsid;
  }
}

/**
 * Cleans-up all symbols.
 */
static void symbols_cleanup( void ) {
  verbose_print_statistics( &print_statistics );
  ht_table_cleanup( &header_set, /*free_fn=*/NULL );
  ht_table_cleanup(
    &tidy_symbol_set, POINTER_CAST( ht_free_fn_t, &symbol_cleanup )
  );
}

/**
 * Adds a symbol for a declaration that isn't a definition (e.g., a function
 * prototype in a header).
 *
 * @note Only the canonical (first) declaration is considered.
 *
 * @param dec_csr The declaration's cursor.
 * @param dec_loc The source location of \a dec_csr.
 * @param svd The symbols_visit_data to use.
 *
 * @sa symbols_print_undefined()
 */
static void symbol_add_dec( CXCursor dec_csr, CXSourceLocation dec_loc,
                            symbols_visit_data *svd ) {
  assert( svd != NULL );

  if ( !tidy_Cursor_isForwardDeclarable( dec_csr ) )
    return;

  CXCursor const canon_dec_csr = clang_getCanonicalCursor( dec_csr );
  if ( !clang_equalCursors( dec_csr, canon_dec_csr ) )
    return;

  tidy_symbol *const sym = symbol_add( dec_csr, svd );
  if ( sym == NULL )
    return;                             // e.g., excluded

  if ( sym->parent_sym == NULL )
    sym->parent_sym = svd->curr_sym;

  if ( sym->is_root && sym->def_loc.path == NULL )
    symbol_add_def( sym, dec_loc, svd );
}

/**
 * Visits each symbol in a translation unit.
 *
 * @param cursor The cursor for the symbol in the AST being visited.
 * @param parent The parent cursor of \a cursor.
 * @param data A pointer to a symbols_visit_data.
 * @return Always returns `CXChildVisit_Continue`.
 */
static enum CXChildVisitResult symbols_visitor( CXCursor cursor,
                                                CXCursor parent,
                                                CXClientData data ) {
  assert( data != NULL );
  symbols_visit_data *const svd = data;

  enum CXCursorKind const kind = clang_getCursorKind( cursor );
  tidy_symbol *const      prev_svd_curr_sym = svd->curr_sym;
  CXSourceLocation const  sym_loc = clang_getCursorLocation( cursor );

  if ( clang_Location_isInSystemHeader( sym_loc ) ) {
    if ( svd->source->lang == CXLanguage_C ) {
      //
      // Symbols in standard headers can be skipped entirely in C.
      //
      goto skip;
    }
    else {
      //
      // In C++, they can't be skipped entirely because template code can
      // contain OverloadedDeclRefs that we need for argument-dependent lookup
      // (ADL).
      //
    }

    enum CXCursorKind const parent_kind = clang_getCursorKind( parent );
    if ( parent_kind == CXCursor_TranslationUnit ) {
      //
      // For a top-level cursor, skip it (and its whole subtree) if its file
      // was already included by an earlier translation unit.
      //
      CXFile const sym_file = tidy_getFileLocation_File( sym_loc );
      if ( file_previously_seen( sym_file, svd ) )
        goto skip;
    }

    if ( kind == CXCursor_OverloadedDeclRef ) {
      //
      // An unqualified call in a standard header, e.g., "swap(a,b)" inside a
      // template, can't be resolved until the template is instantiated because
      // its arguments depend on a template parameter.  Libclang represents the
      // call as an OverloadedDeclRef that has only a name, e.g., "swap".
      //
      // When the template is later instantiated with a user-defined type,
      // argument-dependent lookup (ADL) can find a user function of that name
      // that nothing in user code calls directly.
      //
      // So remember just the name here; it's later combined with the user-
      // defined types collected by cxx_adl_type_set_add() to decide which user
      // functions to treat as roots.
      //
      CXString const    name_cxs = clang_getCursorSpelling( cursor );
      char const *const name = clang_getCString( name_cxs );
      if ( name != NULL && name[0] != '\0' )
        cxx_adl_name_set_add( name );
      clang_disposeString( name_cxs );
    }
  }
  else {
    ++svd->visit_id;

    if ( IS_VERBOSE( CURSORS ) )
      verbose_print_cursor( cursor );   // LCOV_EXCL_LINE

    bool const is_expression = clang_isExpression( kind );

    if ( svd->source->lang == CXLanguage_CPlusPlus &&
        (is_expression || clang_isDeclaration( kind )) ) {
      //
      // Remember the user-defined classes used as template arguments so we can
      // later tell which user functions ADL could find from standard library
      // templates.
      //
      CXType const type = clang_getCursorType( cursor );
      cxx_adl_type_set_add( type );
    }

    //
    // A local variable's initializing expression must be referenced from the
    // enclosing function, not the variable being initialized.  Consider:
    //
    //      void f( int x ) {
    //        int unused_var = x;
    //      }
    //
    //      int main() {
    //        f( 42 );
    //      }
    //
    // If we didn't handle this case specially, we'd get the following symbols
    // and what they reference:
    //
    //      f() -> {}
    //      main() [root] -> { f() }
    //      x -> {}
    //      unused_var -> { x }
    //
    // Note that unused_var references x.  Since nothing references unused_var,
    // it's correctly left as unused.  But unused_var is the only symbol that
    // has a reference to x, so x would be left as unused also --- incorrectly.
    //
    // Instead, we need to make the enclosing function f() reference x.  If f()
    // is used, then x is used to initialize unused_var.  That unused_var is
    // unused is irrelevant to x.
    //
    // By setting curr_sym to be curr_sym's parent, f(), we'd get the following
    // symbols and what they reference:
    //
    //      f() -> { x }
    //      main() [root] -> { f() }
    //      x -> {}
    //      unused_var -> {}
    //
    // Now, f() references x, so it would be marked used --- correctly.
    //
    if ( is_expression && svd->curr_sym != NULL && !svd->curr_sym->is_scope &&
         tidy_Cursor_isLocalVarDecl( parent ) ) {
      svd->curr_sym = svd->curr_sym->parent_sym;
    }

    if ( cursor_is_definition( cursor ) ) {
      tidy_symbol *const sym = symbol_add( cursor, svd );
      if ( likely( sym != NULL ) ) {
        symbol_add_def( sym, sym_loc, svd );
        sym->is_defined = true;
        if ( sym->parent_sym == NULL )
          sym->parent_sym = svd->curr_sym;
        symbol_update_dec_loc( sym, cursor, sym_loc, svd );
        cursor_visit_def( cursor, sym, svd );
      }
    }
    else {
      cursor_visit_ref( cursor, svd );
      symbol_add_dec( cursor, sym_loc, svd );
    }
  }

  clang_visitChildren( cursor, &symbols_visitor, svd );
  svd->curr_sym = prev_svd_curr_sym;

skip:
  return CXChildVisit_Continue;
}

/**
 * Compares two tidy_header objects.
 *
 * @note The \ref tidy_header::tsid "tsid" field is intentionally not compared.
 *
 * @param i_hdr The first tidy_header.
 * @param j_hdr The second tidy_header.
 * @return Returns a number less than 0, 0, or greater than 0 if \a i_hdr is
 * less than, equal to, or greater than \a j_hdr, respectively.
 */
NODISCARD
static int tidy_header_cmp( tidy_header const *i_hdr,
                            tidy_header const *j_hdr ) {
  int const cmp = tidy_FileUniqueID_cmp( &i_hdr->fuid, &j_hdr->fuid );
  if ( cmp != 0 )
    return cmp;
  return (i_hdr->lang > j_hdr->lang) - (i_hdr->lang < j_hdr->lang);
}

/**
 * Calculates the hash of \a hdr.
 *
 * @note The \ref tidy_header::tsid "tsid" field is intentionally not part of
 * the hash.
 *
 * @param hdr The tidy_header to calculate the hash for.
 * @return Returns said hash.
 */
NODISCARD
static ht_hash_val_t tidy_header_hash( tidy_header const *hdr ) {
  fnv1a_t hash = tidy_FileUniqueID_hash( &hdr->fuid );
  hash = FNV1A_PRIME * (hash ^ STATIC_CAST( fnv1a_t, hdr->lang ));
  return STATIC_CAST( ht_hash_val_t, hash );
}

/**
 * Visits a `CXCursor_Constructor` kind of cursor.
 *
 * @param ctor_csr The constructor's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_Constructor( CXCursor ctor_csr, symbols_visit_data *svd ) {
  CXCursor const cls_csr = clang_getCursorSemanticParent( ctor_csr );
  if ( tidy_Cursor_isClassDecl( cls_csr ) ) {
    assert( svd != NULL );
    svd_cxx_constructor const prev_svd_cxx_constructor = svd->cxx_constructor;
    svd->cxx_constructor.ctor_csr = ctor_csr;
    tidy_visitClassChildren( cls_csr, &cxx_def_ctor_add_ref_visitor, svd );
    svd->cxx_constructor = prev_svd_cxx_constructor;
  }
}

/**
 * Visits a `CXCursor_CXXForRangeStmt` kind of cursor.
 *
 * @remarks In a C++ for-range statement, if the object being iterated over is
 * of a class type, we have to add references either to the class's `begin()`
 * and `end()` member functions or `begin(T [const]&)` and `end(T [const]&)`
 * global functions.
 *
 * @param range_stmt_csr The range-for statement's cursor.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXForRangeStmt( CXCursor range_stmt_csr,
                                   symbols_visit_data *svd ) {
  CXCursor range_expr = tidy_Cursor_getFirstExprChild( range_stmt_csr );
  if ( unlikely( clang_Cursor_isNull( range_expr ) ) )
    return;                             // LCOV_EXCL_LINE

  CXType range_type = clang_getCursorType( range_expr );
  bool is_range_const = clang_isConstQualifiedType( range_type );

  range_type = clang_getCanonicalType( range_type );
  range_type = clang_getNonReferenceType( range_type );
  if ( !is_range_const )
    is_range_const = clang_isConstQualifiedType( range_type );

  CXCursor cls_csr = clang_getTypeDeclaration( range_type );
  if ( !tidy_Cursor_isClassDecl( cls_csr ) )
    return;

  cls_csr = clang_getCanonicalCursor( cls_csr );
  svd_cxx_for_range const prev_svd_cxx_for_range = svd->cxx_for_range;
  svd->cxx_for_range = (svd_cxx_for_range){
    .cls_csr = cls_csr,
    .begin_csr = clang_getNullCursor(),
    .end_csr = clang_getNullCursor(),
    .is_const = is_range_const
  };

  //
  // Try looking for T::{ begin | end }() [const] member functions first.
  //
  tidy_visitClassChildren( cls_csr, &cxx_find_begin_end_visitor, svd );

  //
  // If either is not found, try looking for { begin | end }(T [const]&)
  // non-member functions second.
  //
  if ( clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) ||
       clang_Cursor_isNull( svd->cxx_for_range.end_csr ) ) {
    svd->cxx_for_range.begin_csr = clang_getNullCursor();
    svd->cxx_for_range.end_csr = clang_getNullCursor();

    CXCursor scope_csr = clang_getCursorSemanticParent( cls_csr );
    while ( !tidy_Cursor_isInvalid( scope_csr ) ) {
      clang_visitChildren( scope_csr, &cxx_find_begin_end_visitor, svd );
      if ( !clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) &&
           !clang_Cursor_isNull( svd->cxx_for_range.end_csr ) ) {
        break;
      }
      enum CXCursorKind const kind = clang_getCursorKind( scope_csr );
      if ( kind == CXCursor_TranslationUnit )
        break;
      scope_csr = clang_getCursorSemanticParent( scope_csr );
    } // while
  }

  if ( !clang_Cursor_isNull( svd->cxx_for_range.begin_csr ) ) {
    cursor_add_ref( svd->cxx_for_range.begin_csr, svd );
    assert( !clang_Cursor_isNull( svd->cxx_for_range.end_csr ) );
    cursor_add_ref( svd->cxx_for_range.end_csr, svd );
  }

  svd->cxx_for_range = prev_svd_cxx_for_range;
}

/**
 * Visits a `CXCursor_CXXDeleteExpr` kind of cursor.
 *
 * @param delete_csr The `delete` cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXDeleteExpr( CXCursor delete_csr,
                                 symbols_visit_data *svd ) {
  clang_visitChildren( delete_csr, &cxx_DeleteExpr_visitor, svd );
}

/**
 * Visits a `CXCursor_CXXMethod` or `CXCursor_Destructor` kind of cursor.
 *
 * @note In what follows, "function" includes destructor.
 *
 * @remarks When a C++ virtual member function is called, libclang generates an
 * AST containing only the base class's function since it can't know which
 * overridden functions are actually called at run-time.  We therefore have to
 * add references from the base class's function to all the overridden ones.
 *
 * @param method_csr The method's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXMethod( CXCursor method_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXCursor *overridden_csrs;
  unsigned  overridden_csrs_count;
  clang_getOverriddenCursors(
    method_csr, &overridden_csrs, &overridden_csrs_count
  );

  for ( unsigned i = 0; i < overridden_csrs_count; ++i ) {
    tidy_symbol *const base_sym = symbol_add( overridden_csrs[i], svd );
    if ( base_sym != NULL )
      symbol_add_ref( base_sym, svd->curr_sym, svd );
    //
    // Standard library code can call a virtual function on an object of a
    // user-defined derived class, but nothing in the AST shows the call.
    // Therefore, add a reference from the class to the override so it will be
    // marked as used if the class is.
    //
    CXCursor const canon_csr = clang_getCanonicalCursor( overridden_csrs[i] );
    if ( tidy_Cursor_isInSystemHeader( canon_csr ) ) {
      CXCursor const parent_sym = clang_getCursorSemanticParent( method_csr );
      tidy_symbol *const cls_sym = symbol_add( parent_sym, svd );
      if ( cls_sym != NULL ) {
        symbol_add_ref( cls_sym, svd->curr_sym, svd );
      }
      else {
        // If the class was excluded, just mark the method as a root.
        svd->curr_sym->is_root = true;
      }
    }
  } // for

  clang_disposeOverriddenCursors( overridden_csrs );

  cxx_member_def_ctor_add_ref( method_csr, svd->curr_sym, svd );

  enum CXCursorKind const kind = clang_getCursorKind( method_csr );
  switch ( kind ) {
    case CXCursor_CXXMethod:
      if ( tidy_Cursor_isSpellingEqualTo( method_csr, "operator()" ) ) {
        //
        // Libclang's C API doesn't include template instantiations of functors
        // (such as in STL containers) in the AST, so user-defined operator()
        // never get marked as used.
        //
        // As a hack, mark such operators as roots to ensure they (and any
        // symbols they reference) are marked as used.
        //
        svd->curr_sym->is_root = true;
      }
      break;

    case CXCursor_Destructor:
      //
      // A destructor implicitly calls its fields' and base classes'
      // destructors, but libclang's C API gives no reference for them.
      //
      cxx_dtor_member_add_refs( method_csr, svd );
      break;

    default:
      /* suppress warning */;
  } // switch
}

/**
 * Visits a `CXCursor_CXXNewExpr` kind of cursor.
 *
 * @remarks The class's `operator new` is implicitly called, so libclang's C
 * API gives no reference to it.  `operator delete` is also marked since it is
 * called if the constructor throws.
 *
 * @param new_csr The `new` cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_CXXNewExpr( CXCursor new_csr, symbols_visit_data *svd ) {
  CXType  type = clang_getCursorType( new_csr );
          type = clang_getCanonicalType( type );
  if ( unlikely( type.kind != CXType_Pointer ) )
    return;                             // LCOV_EXCL_LINE

  CXType pointee_type = clang_getPointeeType( type );
         pointee_type = clang_getCanonicalType( pointee_type );

  CXCursor const cls_csr = clang_getTypeDeclaration( pointee_type );
  if ( tidy_Cursor_isClassDecl( cls_csr ) )
    cxx_operator_add_refs( cls_csr, "operator new", "operator new[]", svd );
}

/**
 * Visits a `CXCursor_FieldDecl` kind of cursor.
 *
 * @remarks
 * @parblock
 * Ideally, we'd call visit_most_kinds() for a FieldDecl.  The problem is that,
 * given a field declaration like:
 *
 *      #define MAX_ALIGN         8
 *
 *      struct rb_node {
 *        // ...
 *        alignas( MAX_ALIGN ) char data[];
 *      };
 *
 * libclang's AST does _not_ include the `alignas` part, so we can't mark that
 * `MAX_ALIGN` is used.
 *
 * Therefore, we have to fall back to iterating over all tokens of the field's
 * declaration looking for identifiers to mark them as used.
 * @endparblock
 *
 * @param field_csr The field declaration's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_FieldDecl( CXCursor field_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXSourceRange const field_range = tidy_getCursorExtent( field_csr );
  CXTranslationUnit const tu = clang_Cursor_getTranslationUnit( field_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( tu, field_range, &tokens, &token_count );
  if ( unlikely( token_count == 0 ) )
    return;                             // LCOV_EXCL_LINE

  CXCursor          cls_csr = clang_getCursorSemanticParent( field_csr );
  CXString const    field_name_cxs = clang_getCursorSpelling( field_csr );
  char const *const field_name_cs  = clang_getCString( field_name_cxs );

  for ( unsigned i = 0; i < token_count; ++i ) {
    if ( clang_getTokenKind( tokens[i] ) != CXToken_Identifier )
      continue;
    if ( tidy_Token_isSpellingEqualTo( tu, tokens[i], field_name_cs ) )
      continue;

    CXCursor const sym_csr = tidy_Token_getScopedNameCursor(
      tu, tokens, token_count, &i, cls_csr, svd->source->lang
    );
    cursor_add_ref( sym_csr, svd );
  } // for

  clang_disposeTokens( tu, tokens, token_count );

  if ( svd->source->lang == CXLanguage_CPlusPlus ) {
    unsigned const prev_num_refs = svd->curr_sym->ref_sym_set.size;

    cxx_field_def_ctor_add_ref( field_csr, svd );
    cxx_dtors_add_refs( field_csr, svd );

    //
    // If a reference was added to a constructor or destructor of the field's
    // type, either might have side effects, so the field must be considered
    // used if its class is.
    //
    tidy_symbol *const cls_sym = svd->curr_sym->parent_sym;
    if ( cls_sym != NULL && svd->curr_sym->ref_sym_set.size > prev_num_refs )
      symbol_add_ref( cls_sym, svd->curr_sym, svd );
  }
}

/**
 * Visits a `CXCursor_FunctionDecl` kind of cursor in user code.
 *
 * @remarks For C++ only, records the user-defined classes among the function's
 * parameter types so that cxx_symbols_mark_adl_roots() can later determine
 * whether template code in a standard header could call the function via ADL.
 *
 * @param fn_csr The function's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_FunctionDecl( CXCursor fn_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  fwd_dec_visit_def( fn_csr, svd );

  if ( svd->source->lang != CXLanguage_CPlusPlus || svd->curr_sym == NULL )
    return;

  CXType const fn_type = clang_getCursorType( fn_csr );
  int const num_params = clang_getNumArgTypes( fn_type );
  for ( int i = 0; i < num_params; ++i ) {
    CXType const arg_type = clang_getArgType( fn_type, (unsigned)i );
    cxx_adl_collect_classes(
      arg_type, /*skip_top=*/false, &svd->curr_sym->cxx_adl_usr_set
    );
  } // for
}

/**
 * Visits a `CXCursor_MacroDefinition` kind of cursor.
 *
 * @remarks
 * @parblock
 * We have to iterate over all tokens of the macro's definition looking for
 * identifiers to mark them as used.  For example, if a header contains:
 *
 *      #define POINTER_CAST(T,EXPR)    ((T)(uintptr_t)(EXPR))
 *
 * it should also `#include <stdint.h>` because `uintptr_t` is used.  The user
 * of the macro shouldn't have to know or care about the declaration, nor be
 * forced to `#include <stdint.h>` explicitly.
 * @endparblock
 *
 * @param macro_csr The macro definition's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_MacroDefinition( CXCursor macro_csr, 
                                   symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXSourceRange const macro_range = clang_getCursorExtent( macro_csr );

  CXToken *tokens;
  unsigned token_count;
  clang_tokenize( svd->source->tu, macro_range, &tokens, &token_count );
  if ( unlikely( token_count == 0 ) )
    return;                             // LCOV_EXCL_LINE

  //
  // While iterating over all tokens of the macro, we have to skip identifers
  // of macro parameters for function-like macros because those are obviously
  // defined by the macro itself.  To skip them, we first have to collect the
  // set of them.
  //
  hash_table_t param_set;
  ht_table_init(
    &param_set, HT_DINT, 2.0, 10,
    POINTER_CAST( ht_cmp_fn_t, &strcmp ),
    POINTER_CAST( ht_hash_fn_t, &fnv1a_s )
  );

  unsigned i = clang_Cursor_isMacroFunctionLike( macro_csr ) ?
    macro_get_params( svd->source->tu, tokens, token_count, &param_set ) :
    1;                                  // tokens[0] = macro name; start at 1

  for ( ; i < token_count; ++i ) {
    CXCursor const sym_csr = macro_Token_getScopedNameCursor(
      svd->source->tu, tokens, token_count, &i, &param_set, svd->source->lang
    );
    if ( !tidy_Cursor_isInvalid( sym_csr ) )
      cursor_add_ref( sym_csr, svd );
  } // for

  ht_table_cleanup( &param_set, /*free_fn=*/NULL );
  clang_disposeTokens( svd->source->tu, tokens, token_count );
}

/**
 * Visits most kinds of reference cursor since they are handled the same way.
 *
 * @param cursor The cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_most_Ref( CXCursor cursor, symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXCursor const ref_csr = clang_getCursorReferenced( cursor );
  if ( tidy_Cursor_isInvalid( ref_csr ) ) {
    if ( svd->source->lang == CXLanguage_CPlusPlus ) {
      enum CXCursorKind const kind = clang_getCursorKind( cursor );
      if ( kind == CXCursor_MemberRefExpr )
        cxx_member_add_ref( cursor, svd );
    }
    return;
  }

  fwd_dec_visit_ref( ref_csr, svd );

  tidy_symbol *const ref_sym = symbol_add( ref_csr, svd );
  if ( ref_sym != NULL ) {
    symbol_add_ref( svd->curr_sym, ref_sym, svd );
    if ( svd->source->lang == CXLanguage_CPlusPlus )
      cxx_member_def_ctor_add_ref( ref_csr, ref_sym, svd );
  }

  if ( svd->curr_sym == NULL )
    return;

  CXCursor const sem_parent = clang_getCursorSemanticParent( ref_csr );
  enum CXCursorKind const sem_parent_kind = clang_getCursorKind( sem_parent );
  if ( sem_parent_kind == CXCursor_EnumDecl ) {
    //
    // Special case: if any enumeration constant is referenced, add a reference
    // to the enumeration declaration so if any enumeration constant is used,
    // the declaration will be also.
    //
    cursor_add_ref( sem_parent, svd );
  }
}

/**
 * Visits a `CXCursor_OverloadedDeclRef` kind of cursor.
 *
 * @remarks We have to iterate over all overloaded functions since calling
 * clang_getCursorReferenced() on a CXCursor_OverloadedDeclRef returns an
 * invalid or null cursor.
 *
 * @param overloaded_csr The overloaded definition's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_OverloadedDeclRef( CXCursor overloaded_csr,
                                     symbols_visit_data *svd ) {
  assert( svd != NULL );
  assert( svd->source->lang == CXLanguage_CPlusPlus );

  unsigned const num_decls = clang_getNumOverloadedDecls( overloaded_csr );
  for ( unsigned i = 0; i < num_decls; ++i ) {
    CXCursor dec_csr = clang_getOverloadedDecl( overloaded_csr, i );
    if ( tidy_Cursor_isInvalid( dec_csr ) )
      continue;                         // LCOV_EXCL_LINE
    dec_csr = clang_getCanonicalCursor( dec_csr );
    fwd_dec_visit_ref( dec_csr, svd );
    cursor_add_ref( dec_csr, svd );
  } // for
}

/**
 * Visits a `CXCursor_VarDeclRef` kind of cursor.
 *
 * @remarks For C++ only, if a variable of a class type is declared and the
 * class has a constructor, mark the variable as a root since the constructor
 * may have side effects.
 *
 * @note A local variable's initializer must be referred to by the enclosing
 * function, not to the variable.  That redirection can't be done here since it
 * must happen while the variable's children are visited; see
 * symbols_visitor().
 *
 * @param var_decl_csr The variable declaration's cursor to visit.
 * @param svd The symbols_visit_data to use.
 */
static void visit_VarDecl( CXCursor var_decl_csr, symbols_visit_data *svd ) {
  if ( svd->source->lang != CXLanguage_CPlusPlus )
    return;

  CXCursor const cls_csr = tidy_Cursor_getUnderlyingTypeDecl( var_decl_csr );
  if ( !tidy_Cursor_isClassDecl( cls_csr ) )
    return;

  tidy_ConstructorInfo ctor_info;
  bool const has_ctor = tidy_Cursor_getConstructorInfo( cls_csr, &ctor_info );

  unsigned const prev_num_refs =
    svd->curr_sym != NULL ? svd->curr_sym->ref_sym_set.size : 0;

  //
  // Get whether the variable is explicitly initialized.  Given:
  //
  //      struct c_int {
  //        int _n;
  //      };
  //
  //      struct cxx_int {
  //        cxx_int( int n ) : _n{ n } { }
  //        int _n;
  //      };
  //
  // then for these cases:
  //                                    // has_ctor? num_args is_init_explicit?
  //      c_int c = { 42 };             //   false     -1       false
  //      cxx_int x;                    //   true       0       false
  //      cxx_int y{ 42 };              //   true       1       true
  //      cxx_int z = cxx_int{ 42 };    //   true      -1       true
  //
  CXCursor const init_csr = tidy_Cursor_getFirstExprChild( var_decl_csr );
  int const num_args = clang_Cursor_getNumArguments( init_csr );
  bool const is_init_explicit = has_ctor && num_args != 0;

  if ( !is_init_explicit ) {
    if ( has_ctor && !clang_Cursor_isNull( ctor_info.def_ctor_csr ) ) {
      //
      // Its class has a default constructor: add a reference to it.
      //
      cursor_add_ref( ctor_info.def_ctor_csr, svd );
    }
    else {
      //
      // Its class has no default constructor, but it might have members that
      // have default constructors: add references to those, if any.
      //
      tidy_visitClassChildren(
        cls_csr, &cxx_field_def_ctor_add_ref_visitor, svd
      );
    }
  }

  //
  // If the variable's type is a template specialization (e.g., std::vector<S>),
  // add default constructor references for any template type arguments.
  //
  CXType const var_type = clang_getCursorType( var_decl_csr );
  cxx_var_template_args_def_ctor_add_ref( var_type, svd );

  //
  // Add references to the variable's class's destructor and its base class
  // destructors, if any.
  //
  cxx_dtors_add_refs( var_decl_csr, svd );

  //
  // If the variable was either explicitly initialized or a reference was added
  // to a default constructor or destructor, the variable has to be treated as
  // used even if it's apparently unused since its constructor or destructor
  // might have side effects (e.g., locking a mutex).
  //
  if ( is_init_explicit || svd->curr_sym->ref_sym_set.size > prev_num_refs ) {
    tidy_symbol *const parent_sym = svd->curr_sym->parent_sym;
    if ( parent_sym != NULL && tidy_Cursor_inFunctionScope( var_decl_csr ) ) {
      //
      // The constructor and destructor run only if the enclosing function
      // does, so add a reference from the function to the variable.
      //
      symbol_add_ref( parent_sym, svd->curr_sym, svd );
    }
    else {
      //
      // The variable has static storage (global, namespace-scope, or class
      // `static`), so its constructor and destructor run regardless.
      //
      svd->curr_sym->is_root = true;
    }
  }
}

////////// extern functions ///////////////////////////////////////////////////

void cursor_add_ref( CXCursor to_csr, symbols_visit_data *svd ) {
  assert( svd != NULL );

  tidy_symbol *const to_sym = symbol_add( to_csr, svd );
  if ( to_sym != NULL )
    symbol_add_ref( svd->curr_sym, to_sym, svd );
}

void svd_init( symbols_visit_data *svd, tidy_source *source ) {
  assert( svd != NULL );

  bool const ignore_extern =
    config_bool_value( &opt_ignore_externs, source->path );
  if ( !ignore_extern )
    print_unneeded_external = true;

  bool const ignore_undefined =
    config_bool_value( &opt_ignore_undefined, source->path );
  if ( !ignore_undefined )
    print_undefined = true;

  *svd = (symbols_visit_data){
    .source = source,
    .ignore_extern = ignore_extern,
    .ignore_forwards = config_bool_value( &opt_ignore_forwards, source->path ),
    .ignore_undefined = ignore_undefined,
    .cxx_constructor = {
      .ctor_csr = clang_getNullCursor()
    },
    .cxx_for_range = {
      .cls_csr = clang_getNullCursor(),
      .begin_csr = clang_getNullCursor(),
      .end_csr = clang_getNullCursor()
    }
  };
}

tidy_symbol* symbol_add( CXCursor sym_csr, symbols_visit_data const *svd ) {
  assert( svd != NULL );

  sym_csr = clang_getCanonicalCursor( sym_csr );
  if ( cursor_is_excluded( sym_csr, svd->source ) )
    return NULL;

  char *key = tidy_getCursorUSR( sym_csr );
  if ( unlikely( key == NULL ) )
    return NULL;                        // LCOV_EXCL_LINE

  tidy_symbol new_sym = {
    .key = str_move( &key ),
    .name = tidy_Cursor_getScopedSpelling( sym_csr )
  };
  ht_insert_rv_t const hti =
    ht_table_insert( &tidy_symbol_set, &new_sym, sizeof new_sym );
  tidy_symbol *const sym = HT_DINT( hti.entry );
  if ( !hti.inserted ) {
    symbol_cleanup( &new_sym );
    goto done;
  }

  if ( svd->source->lang == CXLanguage_CPlusPlus ) {
    sym->cxx_prototype = tidy_Cursor_getScopedDisplayName( sym_csr );

    //
    // A pure virtual function is never defined by design, so treat its
    // declaration as its definition.  (Deleted and defaulted functions are
    // already definitions to libclang.)
    //
    if ( clang_CXXMethod_isPureVirtual( sym_csr ) ||
         tidy_Cursor_isImplicitMember( sym_csr ) ) {
      sym->is_defined = true;
    }
  }

  //
  // Only a symbol that may be declared without being defined can be undefined.
  // Anything else (e.g., a template parameter) is never defined as such, so
  // treat it as defined.
  //
  if ( !tidy_Cursor_isForwardDeclarable( sym_csr ) )
    sym->is_defined = true;

  //
  // If the symbol's name is "main", we have to mark it as a root since nothing
  // calls main().  But first ensure that it's the program's main() function:
  // in C, anything named "main" must be main(); however, in C++, other things,
  // e.g., member functions, can also be named "main".
  //
  if ( strcmp( sym->name, "main" ) == 0 &&
       tidy_Cursor_isExternalGlobalFunction( sym_csr ) ) {
    sym->is_root = true;
  }

  //
  // A library has no main(), so additional roots can be configured: by name,
  // as all the symbols declared in a public API header, or as the member
  // functions of a root class.  Since sym_csr is
  // the canonical cursor, i.e., the first declaration, a function declared in
  // an API header and defined in a source file is matched via its declaration
  // in the header.
  //
  // Unlike is_root, a configured root isn't unconditionally used: it must also
  // be defined or referenced.
  //
  if ( !sym->is_root && cursor_is_config_root( sym_csr, sym->name ) )
    sym->is_root = true;

  if ( tidy_Cursor_isEnclosingScope( sym_csr ) )
    sym->is_scope = true;

  ht_table_init(
    &sym->ref_sym_set, HT_DPTR, 2.0, 128,
    POINTER_CAST( ht_cmp_fn_t, &symbol_cmp ),
    POINTER_CAST( ht_hash_fn_t, &fnv1a_s )
  );

done:
  //
  // Remember where the (first) declaration is for symbols that are never
  // defined.  This is done only for a symbol that's not (yet) defined and only
  // once by a source for which checking is enabled, no matter which source is
  // visited first, so the result doesn't depend on the order of sources.
  //
  if ( !svd->ignore_undefined && !sym->is_defined &&
       sym->dec_loc.path == NULL ) {
    CXSourceLocation const sloc = clang_getCursorLocation( sym_csr );
    sym->dec_loc = loc_from_SourceLocation( sloc );
  }

  return sym;
}

void symbol_add_ref( tidy_symbol *from_sym, tidy_symbol *to_sym,
                     symbols_visit_data const *svd ) {
  assert( to_sym != NULL );
  assert( svd != NULL );

  //
  // A reference from file scope (e.g., a global initializer) makes the
  // symbol a root.
  //
  if ( from_sym == NULL )
    to_sym->is_root = true;
  else
    ht_table_insert( &from_sym->ref_sym_set, to_sym, 0 );

  symbol_update_ref_origin( from_sym, to_sym, svd );
}

void symbols_init( void ) {
  ASSERT_RUN_ONCE();
  ht_table_init(
    &header_set, HT_DINT, 2.0, 256,
    POINTER_CAST( ht_cmp_fn_t, &tidy_header_cmp ),
    POINTER_CAST( ht_hash_fn_t, &tidy_header_hash )
  );
  ht_table_init(
    &tidy_symbol_set, HT_DINT, 2.0, 4096,
    POINTER_CAST( ht_cmp_fn_t, &symbol_cmp ),
    POINTER_CAST( ht_hash_fn_t, &symbol_hash )
  );
  cxx_symbols_init();
  fwd_decs_init();
  ATEXIT( &symbols_cleanup );
}

unsigned symbols_mark_used( void ) {
  ASSERT_RUN_ONCE();
  cxx_symbols_mark_adl_roots();

  ht_iterator_t iter;

  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_root && !sym->is_defined && !symbol_is_referenced( sym ) )
      sym->is_root = false;
    else if ( sym->is_root )
      symbol_mark_used( sym );
  } // for

  unsigned unused_count = 0;
  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( sym->is_dead )
      continue;
    if ( sym->def_loc.path == NULL || symbol_in_unused_scope( sym ) )
      sym->is_dead = true;
    else if ( symbol_is_unused( sym ) )
      ++unused_count;
  } // for

  return unused_count;
}

unsigned symbols_print_undefined( void ) {
  if ( !print_undefined )
    return 0;

  ht_iterator_t iter;
  unsigned undefined_count = 0;

  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( symbol_is_undefined( sym ) )
      ++undefined_count;
  } // for
  if ( undefined_count == 0 )
    return 0;

  array_t symbol_array = ARRAY_INIT( sizeof(tidy_symbol) );
  array_reserve( &symbol_array, undefined_count );

  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( symbol_is_undefined( sym ) )
      *(tidy_symbol*)array_push_back( &symbol_array ) = *sym;
  } // for

  array_qsort(
    &symbol_array,
    POINTER_CAST( array_cmp_fn_t, &symbol_cmp_by_dec_loc )
  );

  char const *prev_path = "";
  for ( size_t i = 0; i < symbol_array.len; ++i ) {
    tidy_symbol const *const sym = array_at_nc( &symbol_array, i );
    if ( strcmp( sym->dec_loc.path, prev_path ) != 0 ) {
      prev_path = sym->dec_loc.path;
      symbol_print_path( sym, offsetof( tidy_symbol, dec_loc ) );
      EPUTS( ": undefined symbols:\n" );
    }
    symbol_print( sym, offsetof( tidy_symbol, dec_loc ) );
    EPUTC( '\n' );
  } // for

  array_cleanup( &symbol_array, /*free_fn=*/NULL );
  return undefined_count;
}

void symbols_print_unused( unsigned unused_count ) {
  if ( unused_count == 0 )
    return;

  array_t symbol_array = ARRAY_INIT( sizeof(tidy_symbol) );
  array_reserve( &symbol_array, unused_count );

  ht_iterator_t iter;
  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( symbol_is_unused( sym ) )
      *(tidy_symbol*)array_push_back( &symbol_array ) = *sym;
  } // for
  assert( symbol_array.len == unused_count );

  array_qsort(
    &symbol_array,
    POINTER_CAST( array_cmp_fn_t, &symbol_cmp_by_def_loc )
  );

  char const *prev_path = "";
  for ( size_t i = 0; i < symbol_array.len; ++i ) {
    tidy_symbol const *const sym = array_at_nc( &symbol_array, i );
    if ( strcmp( sym->def_loc.path, prev_path ) != 0 ) {
      prev_path = sym->def_loc.path;
      symbol_print_path( sym, offsetof( tidy_symbol, def_loc ) );
      EPUTS( ": unused symbols:\n" );
    }
    symbol_print( sym, offsetof( tidy_symbol, def_loc ) );
    EPUTC( '\n' );
  } // for

  array_cleanup( &symbol_array, /*free_fn=*/NULL );
}

unsigned symbols_print_unneeded_external( void ) {
  if ( !print_unneeded_external )
    return 0;

  ht_iterator_t iter;
  unsigned unneeded_count = 0;

  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( symbol_is_unneeded_extern( sym ) )
      ++unneeded_count;
  } // for
  if ( unneeded_count == 0 )
    return 0;

  array_t symbol_array = ARRAY_INIT( sizeof(tidy_symbol) );
  array_reserve( &symbol_array, unneeded_count );

  ht_iterator_init( &iter, &tidy_symbol_set );
  for ( tidy_symbol const *sym; (sym = ht_iterator_next( &iter )) != NULL; ) {
    if ( symbol_is_unneeded_extern( sym ) )
      *(tidy_symbol*)array_push_back( &symbol_array ) = *sym;
  } // for

  array_qsort(
    &symbol_array,
    POINTER_CAST( array_cmp_fn_t, &symbol_cmp_by_def_loc )
  );

  char const *prev_path = "";
  for ( size_t i = 0; i < symbol_array.len; ++i ) {
    tidy_symbol const *const sym = array_at_nc( &symbol_array, i );
    if ( strcmp( sym->def_loc.path, prev_path ) != 0 ) {
      prev_path = sym->def_loc.path;
      symbol_print_path( sym, offsetof( tidy_symbol, def_loc ) );
      EPUTS( ": unneeded external linkage:\n" );
    }
    symbol_print( sym, offsetof( tidy_symbol, def_loc ) );

    if ( sym->ext_link.header_dec_loc.path != NULL ) {
      EPUTS( " (delete or move declaration at " );
      color_start( stderr, sgr_locus );
      EPRINTF( "\"%s\":%u,%u", sym->ext_link.header_dec_loc.path,
        sym->ext_link.header_dec_loc.line, sym->ext_link.header_dec_loc.col );
      color_end( stderr, sgr_locus );
      EPUTC( ')' );
    }
    EPUTC( '\n' );
  } // for

  array_cleanup( &symbol_array, /*free_fn=*/NULL );
  return unneeded_count;
}

unsigned symbols_visit( symbols_visit_data *svd ) {
  assert( svd != NULL );

  CXCursor const cursor = clang_getTranslationUnitCursor( svd->source->tu );
  clang_visitChildren( cursor, &symbols_visitor, svd );
  return fwd_decs_print_unneeded( svd->source );
}

///////////////////////////////////////////////////////////////////////////////

/** @} */

/* vim:set et sw=2 ts=2: */
