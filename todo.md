# Terminal soft-wrapping

Mirror each logical (unwrapped) terminal line as one buffer line, instead of one buffer line per
Ghostty row, and let Neovim wrap it for display.

## Why

- Yanking from a terminal buffer no longer hard-wraps long lines at the terminal's width
  ([#30117](https://github.com/neovim/neovim/issues/30117)).
- A resize no longer changes any scrollback line, so window `topline`s, cursors, marks and extmarks
  stay on the same text without help. Today only `topline` and the cursor are kept, by the anchors in
  `anchor_window_views()`, `track_buffer_cell()` and `restore_window_views()`, which can then be
  removed. Marks and extmarks in the scrollback still drift on resize.
- A resize no longer rewrites the whole scrollback (`mirror_scrollback_rows()` over all history rows
  in the resized branch of `refresh_scrollback()`), which is O(scrollback) per resize today.
- A window narrower than the terminal (the terminal's size is the largest of the windows that show
  it) wraps lines at its own width instead of cutting them off.

## Ghostty side

- `GHOSTTY_ROW_DATA_WRAP` says a row soft-wraps into the next one, and
  `GHOSTTY_ROW_DATA_WRAP_CONTINUATION` says it continues the previous one (`ghostty/vt/screen.h`).
  These are enough to group rows into logical lines.
- `GHOSTTY_CELL_WIDE_SPACER_HEAD` marks the blank cell left at the end of a soft-wrapped row when a
  wide character didn't fit. It must be dropped when joining rows, like `fetch_row_cells()` already
  drops spacer tails.

## Neovim side

The terminal code assumes `buffer line = Ghostty screen row + 1` in many places. Each needs a
row-to-(line, column) mapping instead.

- **Mirroring** (`mirror_scrollback_rows()`, `refresh_scrollback()`, `delete_scrollback_lines()`):
  history is appended and pruned by logical line. A pruned row can be in the middle of a logical
  line, so the first buffer line may have to be trimmed instead of deleted.
- **Scrollback anchors** (`set_scrollback_anchors()`, `count_dropped_rows()`): they count rows, so
  they need converting to lines, or a separate row count per buffer line.
- **The active screen** (`refresh_screen()`, `row_to_linenr()`): a logical line can start in history
  and continue on screen, so the boundary line is shared. An invalid row means re-rendering its whole
  logical line. Keep a per-line table of rows (or start rows) for the screen and the boundary line.
- **Cursor** (`terminal_check_cursor()`, `adjust_topline_cursor()`): map the terminal cursor's
  `(row, col)` to `(line, byte column)` through the wrap.
- **Mouse** (`mouse_action()`, `send_mouse_event()`): window positions map to buffer positions
  through Neovim's wrapping, then to terminal cells through the row mapping. Check `mouse_spec.lua`.
- **Highlights** (`terminal_get_line_attributes()`): attributes are fetched per row, capped at
  `TERM_ATTRS_MAX`. They have to be joined across the rows of a logical line.
- **Window options**: terminal windows get `'nowrap'` in `runtime/lua/vim/_core/defaults.lua:706`.
  That would become `'wrap'`. Check `'linebreak'`, `'breakindent'` and `'showbreak'`, which would
  change how lines look compared to Ghostty. The `w_leftcol` reset in `refresh_terminal()` and the
  horizontal scroll test (`#35331` in `window_spec.lua`) may no longer apply.
- **`'scrollback'`**: it counts buffer lines, while Ghostty's `SCROLLBACK_MAX_LINES` counts rows. The
  two limits no longer match once a line can span several rows.
- **Alternate screen**: it doesn't reflow and has no scrollback, so keeping one line per row there
  may be simpler.

## Tests

- Expectations with wrapped rows change across `test/functional/terminal/` (`window_spec.lua`,
  `scrollback_spec.lua`, `mouse_spec.lua`, `cursor_spec.lua`, `tui_spec.lua`).
- Add: yanking a wrapped line gives one line; marks and extmarks in the scrollback survive a resize;
  a window narrower than the terminal wraps instead of cutting off.
- The test `keeps the view on the same lines when its width changes` should still pass without the
  anchors.
