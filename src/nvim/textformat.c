// textformat.c: text formatting functions

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nvim/ascii_defs.h"
#include "nvim/buffer_defs.h"
#include "nvim/change.h"
#include "nvim/charset.h"
#include "nvim/cursor.h"
#include "nvim/drawscreen.h"
#include "nvim/eval.h"
#include "nvim/eval/typval.h"
#include "nvim/eval/typval_defs.h"
#include "nvim/eval/vars.h"
#include "nvim/ex_cmds_defs.h"
#include "nvim/garray.h"
#include "nvim/globals.h"
#include "nvim/indent.h"
#include "nvim/indent_c.h"
#include "nvim/input.h"
#include "nvim/insert.h"
#include "nvim/macros_defs.h"
#include "nvim/mark.h"
#include "nvim/mbyte.h"
#include "nvim/memline.h"
#include "nvim/memory.h"
#include "nvim/message.h"
#include "nvim/move.h"
#include "nvim/ops.h"
#include "nvim/option.h"
#include "nvim/option_defs.h"
#include "nvim/option_vars.h"
#include "nvim/os/input.h"
#include "nvim/plines.h"
#include "nvim/pos_defs.h"
#include "nvim/search.h"
#include "nvim/state_defs.h"
#include "nvim/strings.h"
#include "nvim/textformat.h"
#include "nvim/textobject.h"
#include "nvim/ui.h"
#include "nvim/undo.h"
#include "nvim/vim_defs.h"
#include "nvim/window.h"

/// The lines that format_lines() saves for undo before it first changes them.
typedef struct {
  const pos_T *cursor;  ///< Cursor position for undo, or NULL if the lines were saved.
  linenr_T top;         ///< Line before the lines.
  linenr_T bot;         ///< Line after the lines.
  bool failed;          ///< Whether saving the lines failed.
} FormatUndo;

/// How format_lines() rewraps the cursor line and the next line of its paragraph.
typedef enum {
  kRewrapJoin,   ///< Join the next line.
  kRewrapBreak,  ///< Join the next line and break the result.
  kRewrapNone,   ///< Keep the lines as they are.
} Rewrap;

#include "textformat.c.generated.h"

static bool did_add_space = false;  ///< auto_format() added an extra space
                                    ///< under the cursor

#define WHITECHAR(cc) (ascii_iswhite(cc) \
                       && !utf_iscomposing_first(utf_ptr2char((char *)get_cursor_pos_ptr() + 1)))

/// Return true if format option 'x' is in effect.
/// Take care of no formatting when 'paste' is set.
bool has_format_option(int x)
  FUNC_ATTR_PURE FUNC_ATTR_WARN_UNUSED_RESULT
{
  if (p_paste) {
    return false;
  }
  return vim_strchr(curbuf->b_p_fo, x) != NULL;
}

/// The virtual column where byte "col" of "line" starts, like get_nolist_virtcol() but without
/// inline virtual text.
static colnr_T fmt_virtcol(char *line, colnr_T col)
{
  CharsizeArg csarg;
  CSType cstype = init_charsize_arg(&csarg, curwin, 0, line);
  if (curwin->w_p_list && vim_strchr(p_cpo, kCpoListwm) == NULL) {
    csarg.use_tabstop = true;
  }
  csarg.max_head_vcol = -1;
  StrCharInfo ci = utf_ptr2StrCharInfo(line);
  colnr_T vcol = 0;
  while (true) {
    ClusterInfo cli = utf_ClusterInfo(ci);
    CharSize cs = win_charsize(cstype, vcol, ci.ptr, ci.chr.value, &csarg, cli.cells);
    if (*ci.ptr == NUL || cli.next.ptr - line > col) {
      return vcol + cs.head;
    }
    vcol += cs.width;
    ci = cli.next;
  }
}

/// The byte column of the character of "line" at virtual column "wcol", like coladvance() in
/// Insert mode but without inline virtual text.
static colnr_T fmt_vcol2col(char *line, colnr_T wcol)
{
  CharsizeArg csarg;
  CSType cstype = init_charsize_arg(&csarg, curwin, 0, line);
  StrCharInfo ci = utf_ptr2StrCharInfo(line);
  colnr_T vcol = 0;
  while (vcol <= wcol && *ci.ptr != NUL) {
    ClusterInfo cli = utf_ClusterInfo(ci);
    vcol += win_charsize(cstype, vcol, ci.ptr, ci.chr.value, &csarg, cli.cells).width;
    ci = cli.next;
  }
  colnr_T col = (colnr_T)(ci.ptr - line);
  if (vcol > wcol) {
    col--;
    col -= utf_head_off(line, line + col);
  }
  return MAX(col, 0);
}

/// Length of the comment leader of "line", including a line comment after code when 'cindent' is
/// set.
static colnr_T fmt_leader_len(char *line)
{
  colnr_T leader_len = get_leader_len(line, NULL, false, true);
  if (leader_len == 0 && curbuf->b_p_cin) {
    // Check for a line comment after code.
    int comment_start = check_linecomment(line);
    if (comment_start != MAXCOL) {
      leader_len = get_leader_len(line + comment_start, NULL, false, true);
      if (leader_len != 0) {
        leader_len += comment_start;
      }
    }
  }
  return leader_len;
}

/// Finds where to break "line", scanning backwards from byte "startcol".
///
/// @param c  character to be inserted at "startcol" (can be NUL)
/// @param leader_len  length of the comment leader, which is never broken
/// @param min_col  column where the scan stops
/// @param[out] foundcolp  column of the first blank of the break
/// @param[out] end_foundcolp  column of the first character after the blanks of the break
///
/// @return  false if "line" can't be broken.
static bool find_break(char *line, colnr_T startcol, int c, int textwidth, colnr_T leader_len,
                       colnr_T min_col, colnr_T *foundcolp, colnr_T *end_foundcolp)
{
  const bool fo_multibyte = has_format_option(kFoMbyteBreak);
  const bool fo_rigor_tw = has_format_option(kFoRigorousTw);
  const colnr_T wantcol = fmt_vcol2col(line, (colnr_T)textwidth);
  colnr_T col = startcol;
  colnr_T foundcol = 0;
  colnr_T end_foundcol = 0;
  colnr_T skip_pos = 0;
  colnr_T save_col;
  int cc;

#define FMT_WHITECHAR(cc) (ascii_iswhite(cc) \
                           && !utf_iscomposing_first(utf_ptr2char(line + col + 1)))
#define FMT_INC_COL() \
  do { \
    if (line[col] != NUL) { \
      col += utfc_ptr2len(line + col); \
    } \
  } while (0)
#define FMT_DEC_COL() \
  do { \
    col--; \
    col -= utf_head_off(line, line + col); \
  } while (0)

  // Stop at first entered white when 'formatoptions' has 'v'
  while (col >= min_col) {
    if (col == startcol && c != NUL) {
      cc = c;
    } else {
      cc = utf_ptr2char(line + col);
    }
    if (FMT_WHITECHAR(cc)) {
      // remember position of blank just before text
      colnr_T end_col = col;

      // find start of sequence of blanks
      int wcc = 0;  // counter for whitespace chars
      while (col > 0 && FMT_WHITECHAR(cc)) {
        FMT_DEC_COL();
        cc = utf_ptr2char(line + col);

        // Increment count of how many whitespace chars in this
        // group; we only need to know if it's more than one.
        if (wcc < 2) {
          wcc++;
        }
      }
      if (col == 0 && FMT_WHITECHAR(cc)) {
        break;                        // only spaces in front of text
      }

      // Don't break after a period when 'formatoptions' has 'p' and
      // there are less than two spaces.
      if (has_format_option(kFoPeriodAbbr) && cc == '.' && wcc < 2) {
        continue;
      }

      // Don't break until after the comment leader
      if (col < leader_len) {
        break;
      }

      if (has_format_option(kFoOneLetter)) {
        // do not break after one-letter words
        if (col == 0) {
          break;              // one-letter word at begin
        }
        // do not break "#a b" when 'tw' is 2
        if (col <= leader_len) {
          break;
        }
        save_col = col;
        FMT_DEC_COL();
        cc = utf_ptr2char(line + col);

        if (FMT_WHITECHAR(cc)) {
          continue;                   // one-letter, continue
        }
        col = save_col;
      }

      FMT_INC_COL();

      end_foundcol = end_col + 1;
      foundcol = col;
      if (col <= wantcol) {
        break;
      }
    } else if ((cc >= 0x100 || !utf_allow_break_before(cc)) && fo_multibyte) {
      int ncc;
      bool allow_break;

      // Break after or before a multi-byte character.
      if (col != startcol) {
        // Don't break until after the comment leader
        if (col < leader_len) {
          break;
        }
        save_col = col;
        FMT_INC_COL();
        ncc = utf_ptr2char(line + col);
        allow_break = utf_allow_break(cc, ncc);

        // If we have already checked this position, skip!
        if (col != skip_pos && allow_break) {
          foundcol = col;
          end_foundcol = foundcol;
          if (col <= wantcol) {
            break;
          }
        }
        col = save_col;
      }

      if (col == 0) {
        break;
      }

      ncc = cc;
      save_col = col;

      FMT_DEC_COL();
      cc = utf_ptr2char(line + col);

      if (FMT_WHITECHAR(cc)) {
        continue;                     // break with space
      }
      // Don't break until after the comment leader.
      if (col < leader_len) {
        break;
      }

      col = save_col;
      skip_pos = col;

      allow_break = utf_allow_break(cc, ncc);

      // Must handle this to respect line break prohibition.
      if (allow_break) {
        foundcol = col;
        end_foundcol = foundcol;
      }
      if (col <= wantcol) {
        const bool ncc_allow_break = utf_allow_break_before(ncc);

        if (allow_break) {
          break;
        }
        if (!ncc_allow_break && !fo_rigor_tw) {
          // Enable at most 1 punct hang outside of textwidth.
          if (col == startcol) {
            // We are inserting a non-breakable char, postpone
            // line break check to next insert.
            end_foundcol = foundcol = 0;
            break;
          }

          // Neither cc nor ncc is NUL if we are here, so
          // it's safe to inc_cursor.
          save_col = col;

          FMT_INC_COL();
          cc = ncc;
          ncc = utf_ptr2char(line + col);
          // handle insert
          ncc = (ncc != NUL) ? ncc : c;

          allow_break = utf_allow_break(cc, ncc);

          if (allow_break) {
            // Break only when we are not at end of line.
            end_foundcol = foundcol = ncc == NUL ? 0 : col;
            break;
          }
          col = save_col;
        }
      }
    }
    if (col == 0) {
      break;
    }
    FMT_DEC_COL();
  }

#undef FMT_WHITECHAR
#undef FMT_INC_COL
#undef FMT_DEC_COL

  *foundcolp = foundcol;
  *end_foundcolp = end_foundcol;
  return foundcol != 0;
}

/// Breaks the cursor line at a break found by find_break(), opening a new line after it.
///
/// @param startcol  cursor column when the break was searched
/// @param do_comments  whether to insert a comment leader in the new line
/// @param first_line  whether this is the first break of the text being formatted
/// @param[out] did_do_comment  whether a comment leader was inserted
static void break_line(colnr_T startcol, colnr_T foundcol, colnr_T end_foundcol, int flags,
                       bool do_comments, colnr_T leader_len, int second_indent, bool first_line,
                       bool *did_do_comment)
{
  const bool fo_white_par = has_format_option(kFoWhitePar);
  int orig_col = 0;
  char *saved_text = NULL;
  int cc;

  // Going to break the line, remove any "$" now.
  undisplay_dollar();

  // Offset between cursor position and line break is used by replace
  // stack functions.  MODE_VREPLACE does not use this, and backspaces
  // over the text instead.
  if (State & VREPLACE_FLAG) {
    orig_col = startcol;              // Will start backspacing from here
  } else {
    replace_offset = startcol - end_foundcol;
  }

  // adjust startcol for spaces that will be deleted and
  // characters that will remain on top line
  curwin->w_cursor.col = foundcol;
  while ((cc = gchar_cursor(), WHITECHAR(cc))
         && (!fo_white_par || curwin->w_cursor.col < startcol)) {
    inc_cursor();
  }
  startcol -= curwin->w_cursor.col;
  startcol = MAX(startcol, 0);

  if (State & VREPLACE_FLAG) {
    // In MODE_VREPLACE state, we will backspace over the text to be
    // wrapped, so save a copy now to put on the next line.
    saved_text = xstrnsave(get_cursor_pos_ptr(), (size_t)get_cursor_pos_len());
    curwin->w_cursor.col = orig_col;
    saved_text[startcol] = NUL;

    // Backspace over characters that will move to the next line
    if (!fo_white_par) {
      backspace_until_column(foundcol);
    }
  } else {
    // put cursor after pos. to break line
    if (!fo_white_par) {
      curwin->w_cursor.col = foundcol;
    }
  }

  // Split the line just before the margin.
  // Only insert/delete lines, but don't really redraw the window.
  open_line(FORWARD, OPENLINE_DELSPACES + OPENLINE_MARKFIX
            + (fo_white_par ? OPENLINE_KEEPTRAIL : 0)
            + (do_comments ? OPENLINE_DO_COM : 0)
            + OPENLINE_FORMAT
            + ((flags & INSCHAR_COM_LIST) ? OPENLINE_COM_LIST : 0),
            ((flags & INSCHAR_COM_LIST) ? second_indent : old_indent),
            did_do_comment);
  if (!(flags & INSCHAR_COM_LIST)) {
    old_indent = 0;
  }

  replace_offset = 0;
  if (first_line && !(flags & INSCHAR_COM_LIST)) {
    // This section is for auto-wrap of numeric lists.  When not
    // in insert mode (i.e. format_lines()), the INSCHAR_COM_LIST
    // flag will be set and open_line() will handle it (as seen
    // above).  The code here (and in get_number_indent()) will
    // recognize comments if needed...
    if (second_indent < 0 && has_format_option(kFoQNumber)) {
      second_indent = get_number_indent(curwin->w_cursor.lnum - 1);
    }
    if (second_indent >= 0) {
      if (State & VREPLACE_FLAG) {
        change_indent(INDENT_SET, second_indent, false, true);
      } else if (leader_len > 0 && second_indent - leader_len > 0) {
        int padding = second_indent - leader_len;

        // We started at the first_line of a numbered list
        // that has a comment.  the open_line() function has
        // inserted the proper comment leader and positioned
        // the cursor at the end of the split line.  Now we
        // add the additional whitespace needed after the
        // comment leader for the numbered list.
        for (int i = 0; i < padding; i++) {
          ins_str(S_LEN(" "));
        }
      } else {
        set_indent(second_indent, SIN_CHANGED);
      }
    }
  }

  if (State & VREPLACE_FLAG) {
    // In MODE_VREPLACE state we have backspaced over the text to be
    // moved, now we re-insert it into the new line.
    ins_bytes(saved_text);
    xfree(saved_text);
  } else {
    // Check if cursor is not past the NUL off the line, cindent
    // may have added or removed indent.
    curwin->w_cursor.col += startcol;
    colnr_T len = get_cursor_line_len();
    curwin->w_cursor.col = MIN(curwin->w_cursor.col, len);
  }

  set_can_cindent(true);
  // moved the cursor, don't autoindent or cindent now
  Ins.did_ai = false;
  Ins.did_si = false;
  Ins.can_si = false;
  Ins.can_si_back = false;
  line_breakcheck();
}

/// Format text at the current insert position.
///
/// If the INSCHAR_COM_LIST flag is present, then the value of second_indent
/// will be the comment leader length sent to open_line().
///
/// @param c  character to be inserted (can be NUL)
void internal_format(int textwidth, int second_indent, int flags, bool format_only, int c)
{
  int cc;
  char save_char = NUL;
  bool haveto_redraw = false;
  const bool fo_ins_blank = has_format_option(kFoInsBlank);
  bool first_line = true;
  colnr_T leader_len;
  bool no_leader = false;
  bool do_comments = (flags & INSCHAR_DO_COM);
  int has_lbr = curwin->w_p_lbr;

  // make sure win_charsize() counts correctly
  curwin->w_p_lbr = false;

  // When 'ai' is off we don't want a space under the cursor to be
  // deleted.  Replace it with an 'x' temporarily.
  if (!curbuf->b_p_ai && !(State & VREPLACE_FLAG)) {
    cc = gchar_cursor();
    if (ascii_iswhite(cc)) {
      save_char = (char)cc;
      pchar_cursor('x');
    }
  }

  // Repeat breaking lines, until the current line is not too long.
  while (!got_int) {
    colnr_T startcol;                   // Cursor column at entry
    colnr_T foundcol;                   // column for start of spaces
    colnr_T end_foundcol;               // column for start of word
    bool did_do_comment = false;

    colnr_T virtcol = fmt_virtcol(get_cursor_line_ptr(), curwin->w_cursor.col)
                      + char2cells(c != NUL ? c : gchar_cursor());
    if (virtcol <= (colnr_T)textwidth) {
      break;
    }

    if (no_leader) {
      do_comments = false;
    } else if (!(flags & INSCHAR_FORMAT)
               && has_format_option(kFoWrapComs)) {
      do_comments = true;
    }

    // Don't break until after the comment leader
    leader_len = do_comments ? fmt_leader_len(get_cursor_line_ptr()) : 0;

    // If the line doesn't start with a comment leader, then don't
    // start one in a following broken line.  Avoids that a %word
    // moved to the start of the next line causes all following lines
    // to start with %.
    if (leader_len == 0) {
      no_leader = true;
    }
    if (!(flags & INSCHAR_FORMAT)
        && leader_len == 0
        && !has_format_option(kFoWrap)) {
      break;
    }
    if ((startcol = curwin->w_cursor.col) == 0) {
      break;
    }

    // Find position to break at.
    colnr_T min_col = 0;
    if ((fo_ins_blank || has_format_option(kFoInsVi))
        && !(flags & INSCHAR_FORMAT)
        && curwin->w_cursor.lnum == Ins.start.lnum) {
      min_col = Ins.start.col;
    }
    if (!find_break(get_cursor_line_ptr(), startcol, c, textwidth, leader_len, min_col,
                    &foundcol, &end_foundcol)) {
      break;                            // no spaces, cannot break line
    }

    break_line(startcol, foundcol, end_foundcol, flags, do_comments, leader_len, second_indent,
               first_line, &did_do_comment);
    haveto_redraw = true;
    first_line = false;

    // If a comment leader was inserted, may also do this on a following
    // line.
    if (did_do_comment) {
      no_leader = false;
    }
  }

  if (save_char != NUL) {               // put back space after cursor
    pchar_cursor(save_char);
  }

  curwin->w_p_lbr = has_lbr;

  if (!format_only && haveto_redraw) {
    update_topline(curwin);
    redraw_curbuf_later(UPD_VALID);
  }
}

/// Blank lines, and lines containing only the comment leader, are left
/// untouched by the formatting.  The function returns true in this
/// case.  It also returns true when a line starts with the end of a comment
/// ('e' in comment flags), so that this line is skipped, and not joined to the
/// previous line.  A new paragraph starts after a blank line, or when the
/// comment leader changes.
static int fmt_check_par(linenr_T lnum, int *leader_len, char **leader_flags, bool do_comments)
{
  char *flags = NULL;        // init for GCC
  char *ptr = ml_get(lnum);
  if (do_comments) {
    *leader_len = get_leader_len(ptr, leader_flags, false, true);
  } else {
    *leader_len = 0;
  }

  if (*leader_len > 0) {
    // Search for 'e' flag in comment leader flags.
    flags = *leader_flags;
    while (*flags && *flags != ':' && *flags != COM_END) {
      flags++;
    }
  }

  return *skipwhite(ptr + *leader_len) == NUL
         || (*leader_len > 0 && *flags == COM_END)
         || startPS(lnum, NUL, false);
}

/// @return  true if line "lnum" ends in a white character.
static bool ends_in_white(linenr_T lnum)
{
  char *s = ml_get(lnum);

  if (*s == NUL) {
    return false;
  }
  colnr_T l = ml_get_len(lnum) - 1;
  return ascii_iswhite((uint8_t)s[l]);
}

/// @return  true if the two comment leaders given are the same.
///
/// @param lnum  The first line.  White-space is ignored.
/// @param lnum2  The second line.
///
/// @note the whole of 'leader1' must match 'leader2_len' characters from 'leader2'.
static bool same_leader(linenr_T lnum, int leader1_len, char *leader1_flags, linenr_T lnum2,
                        int leader2_len, char *leader2_flags)
{
  int idx1 = 0;
  int idx2 = 0;

  if (leader1_len == 0) {
    return leader2_len == 0;
  }

  // If first leader has 'f' flag, the lines can be joined only if the
  // second line does not have a leader.
  // If first leader has 'e' flag, the lines can never be joined.
  // If first leader has 's' flag, the lines can only be joined if there is
  // some text after it and the second line has the 'm' flag.
  if (leader1_flags != NULL) {
    for (char *p = leader1_flags; *p && *p != ':'; p++) {
      if (*p == COM_FIRST) {
        return leader2_len == 0;
      }
      if (*p == COM_END) {
        return false;
      }
      if (*p == COM_START) {
        int line_len = ml_get_len(lnum);
        if (line_len <= leader1_len) {
          return false;
        }
        if (leader2_flags == NULL || leader2_len == 0) {
          return false;
        }
        for (p = leader2_flags; *p && *p != ':'; p++) {
          if (*p == COM_MIDDLE) {
            return true;
          }
        }
        return false;
      }
    }
  }

  // Get current line and next line, compare the leaders.
  // The first line has to be saved, only one line can be locked at a time.
  char *line1 = xstrnsave(ml_get(lnum), (size_t)ml_get_len(lnum));
  for (idx1 = 0; ascii_iswhite(line1[idx1]); idx1++) {}
  char *line2 = ml_get(lnum2);
  for (idx2 = 0; idx2 < leader2_len; idx2++) {
    if (!ascii_iswhite(line2[idx2])) {
      if (line1[idx1++] != line2[idx2]) {
        break;
      }
    } else {
      while (ascii_iswhite(line1[idx1])) {
        idx1++;
      }
    }
  }
  xfree(line1);

  return idx2 == leader2_len && idx1 == leader1_len;
}

/// Used for auto-formatting.
///
/// @return  true when a paragraph starts in line "lnum".
///          false when the previous line is in the same paragraph.
static bool paragraph_start(linenr_T lnum)
{
  int leader_len = 0;                // leader len of current line
  char *leader_flags = NULL;         // flags for leader of current line
  int next_leader_len = 0;           // leader len of next line
  char *next_leader_flags = NULL;    // flags for leader of next line

  if (lnum <= 1) {
    return true;                // start of the file
  }
  char *p = ml_get(lnum - 1);
  if (*p == NUL) {
    return true;                // after empty line
  }
  const bool do_comments = has_format_option(kFoQComs);  // format comments
  if (fmt_check_par(lnum - 1, &leader_len, &leader_flags, do_comments)) {
    return true;  // after non-paragraph line
  }

  if (fmt_check_par(lnum, &next_leader_len, &next_leader_flags, do_comments)) {
    return true;  // "lnum" is not a paragraph line
  }

  if (has_format_option(kFoWhitePar) && !ends_in_white(lnum - 1)) {
    return true;                // missing trailing space in previous line.
  }
  if (has_format_option(kFoQNumber) && (get_number_indent(lnum) > 0)) {
    return true;                // numbered item starts in "lnum".
  }
  if (!same_leader(lnum - 1, leader_len, leader_flags, lnum,
                   next_leader_len, next_leader_flags)) {
    return true;                // change of comment leader.
  }
  return false;
}

/// Called after inserting or deleting text: When 'formatoptions' includes the
/// 'a' flag format from the current line until the end of the paragraph.
/// Keep the cursor at the same position relative to the text.
/// The caller must have saved the cursor line for undo, following ones will be
/// saved here.
///
/// @param trailblank  when true also format with trailing blank
/// @param prev_line   may start in previous line
void auto_format(bool trailblank, bool prev_line)
{
  if (!has_format_option(kFoAuto)) {
    return;
  }

  pos_T pos = curwin->w_cursor;
  char *old = get_cursor_line_ptr();

  // may remove added space
  check_auto_format(false);

  // Don't format in Insert mode when the cursor is on a trailing blank, the
  // user might insert normal text next.  Also skip formatting when "1" is
  // in 'formatoptions' and there is a single character before the cursor.
  // Otherwise the line would be broken and when typing another non-white
  // next they are not joined back together.
  bool wasatend = (pos.col == get_cursor_line_len());
  if (*old != NUL && !trailblank && wasatend) {
    dec_cursor();
    int cc = gchar_cursor();
    if (!WHITECHAR(cc) && curwin->w_cursor.col > 0
        && has_format_option(kFoOneLetter)) {
      dec_cursor();
    }
    cc = gchar_cursor();
    if (WHITECHAR(cc)) {
      curwin->w_cursor = pos;
      return;
    }
    curwin->w_cursor = pos;
  }

  // Also skip formatting when the user just typed whitespace in the
  // middle of the line.  Reformatting would join all paragraph lines and
  // re-wrap, consuming the space at the line break point via
  // OPENLINE_DELSPACES.  By deferring, the next non-whitespace character
  // will be inserted adjacent to the space, keeping it protected from
  // being consumed at a line break.  auto_format() will then reformat
  // properly on the next keystroke.
  if (*old != NUL && !trailblank && !wasatend && pos.col > 0
      && (State & MODE_INSERT)) {
    char *line = get_cursor_line_ptr();
    if (WHITECHAR(line[pos.col - 1])) {
      curwin->w_cursor = pos;
      return;
    }
  }

  // With the 'c' flag in 'formatoptions' and 't' missing: only format
  // comments.
  if (has_format_option(kFoWrapComs) && !has_format_option(kFoWrap)
      && get_leader_len(old, NULL, false, true) == 0) {
    return;
  }

  // May start formatting in a previous line, so that after "x" a word is
  // moved to the previous line if it fits there now.  Only when this is not
  // the start of a paragraph.
  if (prev_line && !paragraph_start(curwin->w_cursor.lnum)) {
    curwin->w_cursor.lnum--;
    if (u_save_cursor() == FAIL) {
      return;
    }
  }

  // Do the formatting and restore the cursor position.  "saved_cursor" will
  // be adjusted for the text formatting.
  saved_cursor = pos;
  format_lines(-1, false, NULL);
  curwin->w_cursor = saved_cursor;
  saved_cursor.lnum = 0;

  if (curwin->w_cursor.lnum > curbuf->b_ml.ml_line_count) {
    // "cannot happen"
    curwin->w_cursor.lnum = curbuf->b_ml.ml_line_count;
    coladvance(curwin, MAXCOL);
  } else {
    check_cursor_col(curwin);
  }

  // Insert mode: If the cursor is now after the end of the line while it
  // previously wasn't, the line was broken.  Because of the rule above we
  // need to add a space when 'w' is in 'formatoptions' to keep a paragraph
  // formatted.
  if (!wasatend && has_format_option(kFoWhitePar)) {
    char *linep = get_cursor_line_ptr();
    colnr_T len = get_cursor_line_len();
    if (curwin->w_cursor.col == len) {
      char *plinep = xstrnsave(linep, (size_t)len + 2);
      plinep[len] = ' ';
      plinep[len + 1] = NUL;
      ml_replace(curwin->w_cursor.lnum, plinep, false);
      // remove the space later
      did_add_space = true;
    } else {
      // may remove added space
      check_auto_format(false);
    }
  }

  check_cursor(curwin);
}

/// When an extra space was added to continue a paragraph for auto-formatting,
/// delete it now.  The space must be under the cursor, just after the insert
/// position.
///
/// @param end_insert  true when ending Insert mode
void check_auto_format(bool end_insert)
{
  if (!did_add_space) {
    return;
  }

  int cc = gchar_cursor();
  if (!WHITECHAR(cc)) {
    // Somehow the space was removed already.
    did_add_space = false;
  } else {
    int c = ' ';
    if (!end_insert) {
      inc_cursor();
      c = gchar_cursor();
      dec_cursor();
    }
    if (c != NUL) {
      // The space is no longer at the end of the line, delete it.
      del_char(false);
      did_add_space = false;
    }
  }
}

/// Find out textwidth to be used for formatting:
///      if 'textwidth' option is set, use it
///      else if 'wrapmargin' option is set, use curwin->w_view_width-'wrapmargin'
///      if invalid value, use 0.
///      Set default to window width (maximum 79) for "gq" operator.
///
/// @param ff  force formatting (for "gq" command)
int comp_textwidth(bool ff)
{
  int textwidth = (int)curbuf->b_p_tw;
  if (textwidth == 0 && curbuf->b_p_wm) {
    // The width is the window width minus 'wrapmargin' minus all the
    // things that add to the margin.
    textwidth = curwin->w_view_width - (int)curbuf->b_p_wm;
    textwidth -= win_fdccol_count(curwin);
    textwidth -= curwin->w_scwidth;

    if (curwin->w_p_nu || curwin->w_p_rnu) {
      textwidth -= 8;
    }
  }
  textwidth = MAX(textwidth, 0);
  if (ff && textwidth == 0) {
    textwidth = MIN(curwin->w_view_width - 1, 79);
  }
  return textwidth;
}

/// Implementation of the format operator 'gq'.
///
/// @param keep_cursor  keep cursor on same text char
void op_format(oparg_T *oap, bool keep_cursor)
{
  linenr_T old_line_count = curbuf->b_ml.ml_line_count;

  if (!undo_allowed(curbuf)) {
    return;
  }
  curwin->w_cursor = oap->start;

  if (oap->is_VIsual) {
    // When there is no change: need to remove the Visual selection
    redraw_curbuf_later(UPD_INVERTED);
  }

  if ((cmdmod.cmod_flags & CMOD_LOCKMARKS) == 0) {
    // Set '[ mark at the start of the formatted area
    curbuf->b_op_start = oap->start;
  }

  // For "gw" remember the cursor position and put it back below (adjusted
  // for joined and split lines).
  if (keep_cursor) {
    saved_cursor = oap->cursor_start;
  }

  // "u" puts the cursor back where the "gq" or "gw" command was given.
  format_lines(oap->line_count, keep_cursor, &oap->cursor_start);

  // Leave the cursor at the first non-blank of the last formatted line.
  // If the cursor was moved one line back (e.g. with "Q}") go to the next
  // line, so "." will do the next lines.
  if (oap->end_adjusted && curwin->w_cursor.lnum < curbuf->b_ml.ml_line_count) {
    curwin->w_cursor.lnum++;
  }
  beginline(BL_WHITE | BL_FIX);
  old_line_count = curbuf->b_ml.ml_line_count - old_line_count;
  msgmore(old_line_count);

  if ((cmdmod.cmod_flags & CMOD_LOCKMARKS) == 0) {
    // put '] mark on the end of the formatted area
    curbuf->b_op_end = curwin->w_cursor;
  }

  if (keep_cursor) {
    curwin->w_cursor = saved_cursor;
    saved_cursor.lnum = 0;

    // formatting may have made the cursor position invalid
    check_cursor(curwin);
  }

  if (oap->is_VIsual) {
    FOR_ALL_WINDOWS_IN_TAB(wp, curtab) {
      if (wp->w_old_cursor_lnum != 0) {
        // When lines have been inserted or deleted, adjust the end of
        // the Visual area to be redrawn.
        if (wp->w_old_cursor_lnum > wp->w_old_visual_lnum) {
          wp->w_old_cursor_lnum += old_line_count;
        } else {
          wp->w_old_visual_lnum += old_line_count;
        }
      }
    }
  }
}

/// Implementation of the format operator 'gq' for when using 'formatexpr'.
void op_formatexpr(oparg_T *oap)
{
  if (oap->is_VIsual) {
    // When there is no change: need to remove the Visual selection
    redraw_curbuf_later(UPD_INVERTED);
  }

  if (fex_format(oap->start.lnum, oap->line_count, NUL) != 0) {
    // As documented: when 'formatexpr' returns non-zero fall back to
    // internal formatting.
    op_format(oap, false);
  }
}

/// @param c  character to be inserted
int fex_format(linenr_T lnum, long count, int c)
{
  bool use_sandbox = was_set_insecurely(curwin, kOptFormatexpr, OPT_LOCAL);
  const sctx_T save_sctx = current_sctx;

  // Set v:lnum to the first line number and v:count to the number of lines.
  // Set v:char to the character to be inserted (can be NUL).
  set_vim_var_nr(VV_LNUM, (varnumber_T)lnum);
  set_vim_var_nr(VV_COUNT, (varnumber_T)count);
  set_vim_var_char(c);

  current_sctx = curbuf->b_p_script_ctx[kBufOptFormatexpr];

  // 'formatexpr' may modify the buffer, so it runs without textlock.
  int r = eval_expr_option_number(&curbuf->b_p_fex, use_sandbox);

  set_vim_var_string(VV_CHAR, NULL, -1);
  current_sctx = save_sctx;

  return r;
}

/// Saves the lines of "fu" for undo, if they weren't saved yet.
///
/// @return  false if saving failed.
static bool fmt_save_undo(FormatUndo *fu)
{
  if (fu->cursor == NULL) {
    return !fu->failed;
  }
  pos_T cursor = curwin->w_cursor;
  curwin->w_cursor = *fu->cursor;
  fu->failed = u_save(fu->top, fu->bot) == FAIL;
  curwin->w_cursor = cursor;
  fu->cursor = NULL;
  return !fu->failed;
}

/// Whether line "lnum" + 1 is the line that break_line() opens when it breaks line "lnum" at its
/// end, followed by the text of line "lnum" + 1 from byte "body_col". May be false when the indent
/// of the new line is hard to predict.
///
/// @param leader_len  length of the comment leader of the broken line
/// @param first_line  whether line "lnum" starts its paragraph
static bool fmt_continuation_ok(linenr_T lnum, colnr_T body_col, int flags, bool do_comments,
                                colnr_T leader_len, int second_indent, bool first_line)
{
  if (Ins.did_ai) {
    return false;
  }

  // break_line() runs in Insert mode, with the cursor where it breaks the line.
  const int old_State = State;
  State = MODE_INSERT;
  const pos_T save_cursor = curwin->w_cursor;
  curwin->w_cursor = (pos_T){ .lnum = lnum, .col = ml_get_len(lnum) };

  char *cur = xstrnsave(ml_get(lnum), (size_t)ml_get_len(lnum));
  char *next = xstrnsave(ml_get(lnum + 1), (size_t)ml_get_len(lnum + 1));
  const int second_line_indent = (flags & INSCHAR_COM_LIST) ? second_indent : old_indent;
  InsState ins = Ins;
  NewLineLeader nl = new_line_leader(FORWARD,
                                     OPENLINE_FORMAT
                                     + (do_comments ? OPENLINE_DO_COM : 0)
                                     + ((flags & INSCHAR_COM_LIST) ? OPENLINE_COM_LIST : 0),
                                     second_line_indent, cur, (uint8_t)next[body_col], &ins, NULL);

  // The new line before the text of line "lnum" + 1, and the indent that replaces the white space
  // at its start, or -1 when the white space is kept.
  garray_T prefix;
  ga_init(&prefix, 1, 80);
  int indent = -1;
  bool ok = false;

  // The indent from 'autoindent' and 'smartindent', see open_line().
  if (nl.indent || ins.did_si) {
    indent = nl.indent;
    if (ins.did_si) {
      int sw = get_sw_value(curbuf);
      if (p_sr) {
        indent -= indent % sw;
      }
      indent += sw;
    }
    if (curbuf->b_p_ci) {
      if (indent != indent_size_ts(cur, curbuf->b_p_ts, curbuf->b_p_vts_array)) {
        goto theend;
      }
      ga_concat_len(&prefix, cur, (size_t)getwhitecols(cur));
      indent = -1;
    }
  }
  if (nl.lead_len > 0) {
    ga_concat_len(&prefix, nl.leader, (size_t)nl.lead_len);
    if ((flags & INSCHAR_COM_LIST) && second_line_indent > 0) {
      for (int i = second_line_indent - (nl.indent + nl.lead_len); i > 0; i--) {
        ga_append(&prefix, ' ');
      }
    }
  }

  // The indent from 'lisp', 'cindent' or 'indentexpr', see open_line().
  if (!p_paste && nl.leader == NULL && !use_indentexpr_for_lisp() && curbuf->b_p_lisp
      && curbuf->b_p_ai) {
    goto theend;
  }
  const bool do_indentexpr
    = !p_paste && (nl.do_cindent || (curbuf->b_p_ai && use_indentexpr_for_lisp()));
  bool reindent = do_indentexpr;

  // The indent of the second line of the paragraph, see break_line().
  if (first_line && !(flags & INSCHAR_COM_LIST)) {
    if (second_indent < 0 && has_format_option(kFoQNumber)) {
      second_indent = get_number_indent(lnum);
    }
    if (second_indent >= 0 && leader_len > 0 && second_indent - leader_len > 0) {
      if (do_indentexpr) {
        goto theend;
      }
      for (int i = leader_len; i < second_indent; i++) {
        ga_append(&prefix, ' ');
      }
    } else if (second_indent >= 0) {
      indent = second_indent;
      reindent = false;
    }
  }

  const char *p = prefix.ga_len > 0 ? prefix.ga_data : "";
  int len = prefix.ga_len;
  colnr_T col = 0;
  if (indent >= 0) {
    if (curbuf->b_p_pi) {
      goto theend;
    }
    for (; len > 0 && ascii_iswhite(*p); len--) {
      p++;
    }
    col = (colnr_T)getwhitecols(next);
  }
  ok = body_col - col == len && memcmp(next + col, p, (size_t)len) == 0;
  if (ok && indent >= 0) {
    curwin->w_cursor = (pos_T){ .lnum = lnum + 1, .col = body_col };
    ok = !set_indent(indent, SIN_CHECK);
  }

  // Line "lnum" + 1 is now the new line before 'cindent' or 'indentexpr' indents it, and gets the
  // same indent.
  if (ok && reindent) {
    curwin->w_cursor = (pos_T){ .lnum = lnum + 1, .col = body_col };
    int amount = curbuf->b_p_inde.type != kCallbackNone ? get_expr_indent() : get_c_indent();
    ok = amount < 0 || (!curbuf->b_p_pi && !curbuf->b_p_ci && !set_indent(amount, SIN_CHECK));
  }

theend:
  ga_clear(&prefix);
  xfree(nl.allocated);
  xfree(cur);
  xfree(next);
  curwin->w_cursor = save_cursor;
  State = old_State;
  return ok;
}

/// The rewrap of the cursor line and the next line of its paragraph, which starts with a comment
/// leader of length "next_leader_len".
///
/// @param first_line  whether the cursor line starts its paragraph
static Rewrap fmt_rewrap(int textwidth, int flags, bool do_comments, int next_leader_len,
                         int second_indent, bool first_line)
{
  linenr_T lnum = curwin->w_cursor.lnum;
  char *next = ml_get(lnum + 1);
  colnr_T body_col = (colnr_T)(skipwhite(next + MAX(next_leader_len, 0)) - next);
  colnr_T cur_len = ml_get_len(lnum);
  colnr_T next_len = ml_get_len(lnum + 1);

  // The line that joining the next line gives.
  char *line = xmalloc((size_t)cur_len + (size_t)(next_len - body_col) + 3);
  memcpy(line, ml_get(lnum), (size_t)cur_len);
  next = ml_get(lnum + 1);
  int spaces = join_spaces(line, (size_t)cur_len, next + body_col);
  memset(line + cur_len, ' ', (size_t)spaces);
  memcpy(line + cur_len + spaces, next + body_col, (size_t)(next_len - body_col));
  line[cur_len + spaces + next_len - body_col] = NUL;

  // Break it like internal_format() would, from its last non-blank.
  Rewrap rewrap = kRewrapJoin;
  int has_lbr = curwin->w_p_lbr;
  curwin->w_p_lbr = false;
  colnr_T startcol = MAX((colnr_T)strlen(line) - 1, 0);
  startcol -= utf_head_off(line, line + startcol);
  while (startcol > 0 && ascii_isspace((uint8_t)line[startcol])) {
    startcol--;
    startcol -= utf_head_off(line, line + startcol);
  }
  colnr_T leader_len = do_comments ? fmt_leader_len(line) : 0;
  colnr_T foundcol;
  colnr_T end_foundcol;
  if (startcol > 0
      && fmt_virtcol(line, startcol) + char2cells(utf_ptr2char(line + startcol)) > textwidth
      && find_break(line, startcol, NUL, textwidth, leader_len, 0, &foundcol, &end_foundcol)) {
    colnr_T col = foundcol;
    if (has_format_option(kFoWhitePar)) {
      while (col < startcol && ascii_iswhite((uint8_t)line[col])
             && !utf_iscomposing_first(utf_ptr2char(line + col + 1))) {
        col++;
      }
    }
    rewrap = col == cur_len
             && fmt_continuation_ok(lnum, body_col, flags, do_comments, leader_len, second_indent,
                                    first_line)
             ? kRewrapNone : kRewrapBreak;
  }
  curwin->w_p_lbr = has_lbr;

  xfree(line);
  return rewrap;
}

/// Breaks the cursor line once if it's too long, like an iteration of internal_format().
///
/// @param first_line  whether the cursor line starts its paragraph
///
/// @return  whether the line was broken.
static bool fmt_break_line(FormatUndo *fu, int textwidth, int flags, bool do_comments,
                           int second_indent, bool first_line)
{
  const int old_State = State;

  // put cursor on last non-space
  State = MODE_NORMAL;  // don't go past end-of-line
  coladvance(curwin, MAXCOL);
  while (curwin->w_cursor.col && ascii_isspace(gchar_cursor())) {
    dec_cursor();
  }
  State = old_State;

  int has_lbr = curwin->w_p_lbr;
  curwin->w_p_lbr = false;
  char *line = get_cursor_line_ptr();
  colnr_T startcol = curwin->w_cursor.col;
  colnr_T leader_len = do_comments ? fmt_leader_len(line) : 0;
  colnr_T foundcol;
  colnr_T end_foundcol;
  bool do_break = textwidth > 0 && startcol > 0
                  && (fmt_virtcol(line, startcol) + char2cells(utf_ptr2char(line + startcol))
                      > textwidth)
                  && find_break(line, startcol, NUL, textwidth, leader_len, 0, &foundcol,
                                &end_foundcol)
                  && fmt_save_undo(fu);
  if (do_break) {
    // do the formatting, without 'showmode'
    State = MODE_INSERT;  // for open_line()
    int smd_save = p_smd;
    p_smd = false;
    bool did_do_comment = false;
    break_line(startcol, foundcol, end_foundcol, flags, do_comments, leader_len, second_indent,
               first_line, &did_do_comment);
    State = old_State;
    p_smd = smd_save;
    // Cursor shape may have been updated (e.g. by :normal) in break_line(),
    // so it needs to be updated here.
    ui_cursor_shape();
  }
  curwin->w_p_lbr = has_lbr;
  return do_break;
}

/// Whether a line from "lnum1" to "lnum2" has a line comment.
static bool fmt_has_linecomment(linenr_T lnum1, linenr_T lnum2)
{
  for (linenr_T lnum = lnum1; lnum <= lnum2; lnum++) {
    if (check_linecomment(ml_get(lnum)) != MAXCOL) {
      return true;
    }
  }
  return false;
}

/// Whether breaking line "lnum", without a comment leader, indents the new line with 'cindent',
/// 'indentexpr' or 'lisp'.
static bool fmt_reindents(linenr_T lnum)
{
  if (p_paste) {
    return false;
  }
  return (curbuf->b_p_lisp && curbuf->b_p_ai)
         || ((curbuf->b_p_cin || curbuf->b_p_inde.type != kCallbackNone)
             && in_cinkeys(KEY_OPEN_FORW, ' ', linewhite(lnum)));
}

/// Sets the indent of the first line of a paragraph, the cursor line.
///
/// @return  false if saving for undo failed.
static bool fmt_set_par_indent(FormatUndo *fu, linenr_T first_line)
{
  int indent = 0;  // amount of indent needed

  // Replace indent in first line of a paragraph with minimal
  // number of tabs and spaces, according to current options.
  // For the very first formatted line keep the current
  // indent.
  if (curwin->w_cursor.lnum == first_line) {
    indent = get_indent();
  } else if (curbuf->b_p_lisp) {
    indent = get_lisp_indent();
  } else {
    if (cindent_on()) {
      indent = curbuf->b_p_inde.type != kCallbackNone ? get_expr_indent() : get_c_indent();
    } else {
      indent = get_indent();
    }
  }
  if (!set_indent(indent, SIN_CHECK)) {
    return true;
  }
  if (!fmt_save_undo(fu)) {
    return false;
  }
  set_indent(indent, SIN_CHANGED);
  return true;
}

/// @param line_count  number of lines to format, starting at the cursor position.
///                    when negative, format until the end of the paragraph.
///
/// When "undo_cursor" is NULL, lines after the cursor line are saved for undo,
/// and the caller must have saved the first line.  Otherwise the formatted
/// lines are saved for undo, with the cursor at "undo_cursor", before they're
/// first changed.
///
/// @param avoid_fex  don't use 'formatexpr'
void format_lines(linenr_T line_count, bool avoid_fex, const pos_T *undo_cursor)
{
  bool is_not_par;                  // current line not part of parag.
  bool next_is_not_par;             // next line not part of paragraph
  bool is_end_par;                  // at end of paragraph
  bool prev_is_end_par = false;     // prev. line not part of parag.
  bool next_is_start_par = false;
  int leader_len = 0;               // leader len of current line
  int next_leader_len;              // leader len of next line
  char *leader_flags = NULL;        // flags for leader of current line
  char *next_leader_flags = NULL;   // flags for leader of next line
  bool advance = true;
  int second_indent = -1;           // indent for second line (comment aware)
  bool first_par_line = true;
  int smd_save;
  long count;
  bool need_set_indent = true;      // set indent of next paragraph
  linenr_T first_line = curwin->w_cursor.lnum;
  bool force_format = false;
  const int old_State = State;
  linenr_T par_lnum = 0;            // first line of the paragraph
  bool par_do_comments = false;     // lines of the paragraph get a comment leader
  bool par_incremental = false;     // rewrap the paragraph one line at a time
  FormatUndo fu = {
    .cursor = undo_cursor,
    .top = first_line - 1,
    .bot = first_line + (linenr_T)MAX(line_count, 0),
  };

  // Only change the lines whose wrapping changes, unless 'formatexpr' is
  // used for the joined lines.
  const bool incremental = line_count > 0
                           && (avoid_fex || curbuf->b_p_fex.type == kCallbackNone);

  const int textwidth = comp_textwidth(true);

  // length of a line to force formatting: 3 * 'tw'
  const int max_len = textwidth * 3;

  // check for 'q', '2', 'n' and 'w' in 'formatoptions'
  const bool do_comments = has_format_option(kFoQComs);  // format comments
  int do_comments_list = 0;  // format comments with 'n' or '2'
  const bool do_second_indent = has_format_option(kFoQSecond);
  const bool do_number_indent = has_format_option(kFoQNumber);
  const bool do_trail_white = has_format_option(kFoWhitePar);

  if (!incremental && !fmt_save_undo(&fu)) {
    return;
  }

  // Get info about the previous and current line.
  if (curwin->w_cursor.lnum > 1) {
    is_not_par = fmt_check_par(curwin->w_cursor.lnum - 1,
                               &leader_len, &leader_flags, do_comments);
  } else {
    is_not_par = true;
  }
  next_is_not_par = fmt_check_par(curwin->w_cursor.lnum,
                                  &next_leader_len, &next_leader_flags, do_comments);
  is_end_par = (is_not_par || next_is_not_par);
  if (!is_end_par && do_trail_white) {
    is_end_par = !ends_in_white(curwin->w_cursor.lnum - 1);
  }

  curwin->w_cursor.lnum--;
  for (count = line_count; count != 0 && !got_int; count--) {
    // Advance to next paragraph.
    if (advance) {
      curwin->w_cursor.lnum++;
      prev_is_end_par = is_end_par;
      is_not_par = next_is_not_par;
      leader_len = next_leader_len;
      leader_flags = next_leader_flags;
    }

    // The last line to be formatted.
    if (count == 1 || curwin->w_cursor.lnum == curbuf->b_ml.ml_line_count) {
      next_is_not_par = true;
      next_leader_len = 0;
      next_leader_flags = NULL;
    } else {
      next_is_not_par = fmt_check_par(curwin->w_cursor.lnum + 1,
                                      &next_leader_len, &next_leader_flags, do_comments);
      if (do_number_indent) {
        next_is_start_par =
          (get_number_indent(curwin->w_cursor.lnum + 1) > 0);
      }
    }
    advance = true;
    is_end_par = (is_not_par || next_is_not_par || next_is_start_par);
    if (!is_end_par && do_trail_white) {
      is_end_par = !ends_in_white(curwin->w_cursor.lnum);
    }

    // Skip lines that are not in a paragraph.
    if (is_not_par) {
      if (line_count < 0) {
        break;
      }
    } else {
      if (first_par_line) {
        par_lnum = curwin->w_cursor.lnum;
      }

      // For the first line of a paragraph, check indent of second line.
      // Don't do this for comments and empty lines.
      if (first_par_line
          && (do_second_indent || do_number_indent)
          && prev_is_end_par
          && curwin->w_cursor.lnum < curbuf->b_ml.ml_line_count) {
        if (do_second_indent && !LINEEMPTY(curwin->w_cursor.lnum + 1)) {
          if (leader_len == 0 && next_leader_len == 0) {
            // no comment found
            second_indent =
              get_indent_lnum(curwin->w_cursor.lnum + 1);
          } else {
            second_indent = next_leader_len;
            do_comments_list = 1;
          }
        } else if (do_number_indent) {
          if (leader_len == 0 && next_leader_len == 0) {
            // no comment found
            second_indent =
              get_number_indent(curwin->w_cursor.lnum);
          } else {
            // get_number_indent() is now "comment aware"...
            second_indent =
              get_number_indent(curwin->w_cursor.lnum);
            do_comments_list = 1;
          }
        }
      }

      // When the comment leader changes, it's the end of the paragraph.
      if (curwin->w_cursor.lnum >= curbuf->b_ml.ml_line_count
          || !same_leader(par_incremental ? par_lnum : curwin->w_cursor.lnum,
                          leader_len, leader_flags,
                          curwin->w_cursor.lnum + 1,
                          next_leader_len,
                          next_leader_flags)) {
        // Special case: If the next line starts with a line comment
        // and this line has a line comment after some text, the
        // paragraph doesn't really end.
        if (next_leader_flags == NULL
            || strncmp(next_leader_flags, "://", 3) != 0
            || !fmt_has_linecomment(par_incremental ? par_lnum : curwin->w_cursor.lnum,
                                    curwin->w_cursor.lnum)) {
          is_end_par = true;
        }
      }

      const int flags = INSCHAR_FORMAT
                        + (do_comments ? INSCHAR_DO_COM : 0)
                        + (do_comments && do_comments_list ? INSCHAR_COM_LIST : 0)
                        + (avoid_fex ? INSCHAR_NO_FEX : 0);

      if (incremental && first_par_line) {
        par_do_comments = do_comments && fmt_leader_len(get_cursor_line_ptr()) > 0;
        // 'cindent', 'indentexpr' and 'lisp' indent a new line depending on
        // all the text that's joined after it, unless it has a comment leader.
        par_incremental = par_do_comments || !fmt_reindents(curwin->w_cursor.lnum);
        if (par_incremental && need_set_indent) {
          if (!fmt_set_par_indent(&fu, first_line)) {
            break;
          }
          need_set_indent = false;
          // The leader is compared with the following lines, with its new indent.
          fmt_check_par(par_lnum, &leader_len, &leader_flags, do_comments);
        }
      }

      // If we have got to the end of a paragraph, or the line is
      // getting long, format it.
      if (is_end_par || force_format) {
        if (incremental) {
          if (need_set_indent && !fmt_set_par_indent(&fu, first_line)) {
            break;
          }
          // Like internal_format(), each formatting of a joined paragraph
          // starts with its first line.
          bool first = !par_incremental || curwin->w_cursor.lnum == par_lnum;
          while (fmt_break_line(&fu, textwidth, flags, par_do_comments, second_indent, first)) {
            first = false;
          }
          if (fu.failed) {
            break;
          }
        } else {
          if (need_set_indent) {
            fmt_set_par_indent(&fu, first_line);
          }

          // put cursor on last non-space
          State = MODE_NORMAL;  // don't go past end-of-line
          coladvance(curwin, MAXCOL);
          while (curwin->w_cursor.col && ascii_isspace(gchar_cursor())) {
            dec_cursor();
          }

          // do the formatting, without 'showmode'
          State = MODE_INSERT;         // for open_line()
          smd_save = p_smd;
          p_smd = false;

          insertchar(NUL, flags, second_indent);

          State = old_State;
          p_smd = smd_save;
          // Cursor shape may have been updated (e.g. by :normal) in insertchar(),
          // so it needs to be updated here.
          ui_cursor_shape();
        }

        second_indent = -1;
        // at end of par.: need to set indent of next par.
        need_set_indent = is_end_par;
        if (is_end_par) {
          // When called with a negative line count, break at the
          // end of the paragraph.
          if (line_count < 0) {
            break;
          }
          first_par_line = true;
        }
        force_format = false;
      }

      // When still in same paragraph, join the lines together.  But
      // first delete the leader from the second line.
      if (!is_end_par) {
        Rewrap rewrap = kRewrapJoin;
        if (par_incremental) {
          rewrap = fmt_rewrap(textwidth, flags, par_do_comments, next_leader_len, second_indent,
                              curwin->w_cursor.lnum == par_lnum);
          if (rewrap == kRewrapNone) {
            // Keep the next line, it continues the paragraph.
            advance = false;
            curwin->w_cursor.lnum++;
            first_par_line = false;
            line_breakcheck();
            continue;
          }
        }
        if (!fmt_save_undo(&fu)) {
          break;
        }
        advance = false;
        curwin->w_cursor.lnum++;
        curwin->w_cursor.col = 0;
        if (line_count < 0 && u_save_cursor() == FAIL) {
          break;
        }
        if (next_leader_len > 0) {
          del_bytes(next_leader_len, false, false);
          mark_col_adjust(curwin->w_cursor.lnum, 0, 0, -next_leader_len, 0);
        } else if (second_indent > 0) {   // the "leader" for kFoQSecond
          int indent = (int)getwhitecols_curline();

          if (indent > 0) {
            del_bytes(indent, false, false);
            mark_col_adjust(curwin->w_cursor.lnum, 0, 0, -indent, 0);
          }
        }
        curwin->w_cursor.lnum--;
        if (do_join(2, true, false, false, false) == FAIL) {
          beep_flush();
          break;
        }
        first_par_line = false;
        if (rewrap == kRewrapBreak) {
          fmt_break_line(&fu, textwidth, flags, par_do_comments, second_indent,
                         curwin->w_cursor.lnum == par_lnum);
        } else if (!par_incremental) {
          // If the line is getting long, format it next time
          force_format = get_cursor_line_len() > max_len;
        }
      }
    }
    line_breakcheck();
  }
}
