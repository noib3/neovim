#pragma once

#include <stddef.h>  // IWYU pragma: keep

#include "nvim/insert_defs.h"  // IWYU pragma: keep
#include "nvim/pos_defs.h"  // IWYU pragma: keep
#include "nvim/types_defs.h"  // IWYU pragma: keep

/// flags for open_line()
enum {
  OPENLINE_DELSPACES    = 0x01,  ///< delete spaces after cursor
  OPENLINE_DO_COM       = 0x02,  ///< format comments
  OPENLINE_KEEPTRAIL    = 0x04,  ///< keep trailing spaces
  OPENLINE_MARKFIX      = 0x08,  ///< fix mark positions
  OPENLINE_COM_LIST     = 0x10,  ///< format comments with list/2nd line indent
  OPENLINE_FORMAT       = 0x20,  ///< formatting long comment
  OPENLINE_FORCE_INDENT = 0x40,  ///< use second_line_indent without indent logic
};

/// The indent and comment leader of a line that open_line() opens.
typedef struct {
  int indent;          ///< indent before the leader, without the one 'smartindent' adds
  char *leader;        ///< comment leader, or NULL
  char *allocated;     ///< allocated memory that "leader" points into
  int lead_len;        ///< length of "leader"
  colnr_T col;         ///< column after the leader
  bool no_si;          ///< "Ins.did_si" is reset after the indent is set
  bool do_cindent;     ///< 'cindent' or 'indentexpr' indents the line
} NewLineLeader;

#include "change.h.generated.h"
