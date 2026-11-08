local t = require('test.testutil')
local n = require('test.functional.testnvim')()

local describe, it, before_each, after_each = t.describe, t.it, t.before_each, t.after_each
local clear = n.clear
local command = n.command
local eq = t.eq
local api = n.api
local fn = n.fn
local exec_lua = n.exec_lua
local feed = n.feed
local write_file = t.write_file
local read_file = t.read_file

local fname = 'Xtest-checktime'

--- Writes `text` to the file, dated in the past, and edits it.
local function edit(text)
  write_file(fname, text)
  local past = os.time() - 10
  vim.uv.fs_utime(fname, past, past)
  command('edit ' .. fname)
end

--- Writes `text` to the file and reloads the buffer.
local function reload(text)
  write_file(fname, text)
  command('checktime')
end

describe(':checktime reload', function()
  before_each(function()
    clear()
    command('set autoread')
  end)

  after_each(function()
    os.remove(fname)
  end)

  it('moves extmarks and marks with the text around them', function()
    edit('a\nfoo bar baz\nc\n')
    local ns = api.nvim_create_namespace('test')
    local on_c = api.nvim_buf_set_extmark(0, ns, 2, 0, {})
    local on_baz = api.nvim_buf_set_extmark(0, ns, 1, 8, {})
    command('3mark x')

    reload('new 1\nnew 2\na\nfoo BAR baz\nc\n')

    eq({ 'new 1', 'new 2', 'a', 'foo BAR baz', 'c' }, api.nvim_buf_get_lines(0, 0, -1, true))
    eq({ 4, 0 }, api.nvim_buf_get_extmark_by_id(0, ns, on_c, {}))
    eq({ 3, 8 }, api.nvim_buf_get_extmark_by_id(0, ns, on_baz, {}))
    eq(5, fn.line("'x"))
  end)

  it('pairs up similar lines when lines are inserted or deleted between them', function()
    edit('foo(a)\nbar(b)\nbaz(c)\nqux(d)\n')
    local ns = api.nvim_create_namespace('test')
    local on_a = api.nvim_buf_set_extmark(0, ns, 0, 4, {})
    local on_b = api.nvim_buf_set_extmark(0, ns, 1, 4, {})
    local on_d = api.nvim_buf_set_extmark(0, ns, 3, 4, {})

    reload('foo(a, 1)\ninserted\nbar(b, 2)\nqux(d, 4)\n')

    eq(
      { 'foo(a, 1)', 'inserted', 'bar(b, 2)', 'qux(d, 4)' },
      api.nvim_buf_get_lines(0, 0, -1, true)
    )
    eq({ 0, 4 }, api.nvim_buf_get_extmark_by_id(0, ns, on_a, {}))
    eq({ 2, 4 }, api.nvim_buf_get_extmark_by_id(0, ns, on_b, {}))
    eq({ 3, 4 }, api.nvim_buf_get_extmark_by_id(0, ns, on_d, {}))
  end)

  it('pairs up reindented lines', function()
    edit('a\nb\n')
    local ns = api.nvim_create_namespace('test')
    local on_b = api.nvim_buf_set_extmark(0, ns, 1, 0, {})

    reload('if x then\n  a\n  b\nend\n')

    eq({ 2, 2 }, api.nvim_buf_get_extmark_by_id(0, ns, on_b, {}))
  end)

  it('does not pair up unrelated lines', function()
    edit('first\nabc\nlast\n')
    local ns = api.nvim_create_namespace('test')
    local on_abc = api.nvim_buf_set_extmark(0, ns, 1, 0, { end_col = 3, invalidate = true })

    reload('first\nxyz\nuvw\nlast\n')

    eq(true, api.nvim_buf_get_extmark_by_id(0, ns, on_abc, { details = true })[3].invalid)
  end)

  it('moves the cursor with the text around it', function()
    edit('a\nb\nc\n')
    api.nvim_win_set_cursor(0, { 3, 0 })

    reload('new\na\nb\nc\n')

    eq({ 4, 0 }, api.nvim_win_get_cursor(0))
  end)

  it('sends the changes as buffer updates, without BufReadPost', function()
    edit('a\nb\n')
    exec_lua(function()
      _G.events = {}
      vim.api.nvim_buf_attach(0, false, {
        on_lines = function(_, _, _, first, last, new_last)
          table.insert(_G.events, { 'lines', first, last, new_last })
        end,
        on_reload = function()
          table.insert(_G.events, { 'reload' })
        end,
      })
      for _, event in ipairs({ 'BufReadPost', 'FileChangedShellPost' }) do
        vim.api.nvim_create_autocmd(event, {
          callback = function()
            table.insert(_G.events, { event })
          end,
        })
      end
    end)

    reload('a\nb\nc\n')

    eq({ { 'lines', 2, 2, 3 }, { 'FileChangedShellPost' } }, exec_lua('return _G.events'))
  end)

  it('replaces the whole text if a BufReadPre autocommand matches the file', function()
    edit('a\nb\n')
    exec_lua(function()
      _G.events = {}
      vim.api.nvim_buf_attach(0, false, {
        on_reload = function()
          table.insert(_G.events, 'reload')
        end,
      })
      vim.api.nvim_create_autocmd('BufReadPre', {
        pattern = '*-checktime',
        callback = function()
          table.insert(_G.events, 'BufReadPre')
        end,
      })
      vim.api.nvim_create_autocmd('BufReadPost', {
        callback = function()
          table.insert(_G.events, 'BufReadPost')
        end,
      })
    end)

    reload('a\nb\nc\n')

    eq({ 'a', 'b', 'c' }, api.nvim_buf_get_lines(0, 0, -1, true))
    eq({ 'BufReadPre', 'BufReadPost', 'reload' }, exec_lua('return _G.events'))
  end)

  it('can be undone in one step, and leaves the buffer unmodified', function()
    edit('a\nb\nc\nd\n')
    command('set undolevels=100')

    reload('a\nB\nc\nd\ne\n')

    eq(false, api.nvim_get_option_value('modified', {}))
    eq({ 'a', 'B', 'c', 'd', 'e' }, api.nvim_buf_get_lines(0, 0, -1, true))
    eq(0, #fn.getchangelist()[1])

    feed('u')
    eq({ 'a', 'b', 'c', 'd' }, api.nvim_buf_get_lines(0, 0, -1, true))
    eq(true, api.nvim_get_option_value('modified', {}))

    feed('<C-R>')
    eq({ 'a', 'B', 'c', 'd', 'e' }, api.nvim_buf_get_lines(0, 0, -1, true))
  end)

  it("clears undo past 'undoreload'", function()
    edit('a\nb\nc\n')
    command('set undoreload=2')

    reload('a\nB\nc\n')

    eq({ 'a', 'B', 'c' }, api.nvim_buf_get_lines(0, 0, -1, true))
    eq({}, fn.undotree().entries)
  end)

  it('reloads a nomodifiable or readonly buffer without warnings', function()
    edit('a\n')
    command('setlocal nomodifiable readonly')

    reload('b\n')

    eq({ 'b' }, api.nvim_buf_get_lines(0, 0, -1, true))
    eq('', api.nvim_get_vvar('warningmsg'))
    eq('', api.nvim_get_vvar('errmsg'))
  end)

  it('reloads from and to an empty file', function()
    edit('a\nb\n')

    reload('')
    eq({ '' }, api.nvim_buf_get_lines(0, 0, -1, true))
    eq(1, fn.line('$'))
    command('write!')
    eq('', read_file(fname))

    reload('x\n')
    eq({ 'x' }, api.nvim_buf_get_lines(0, 0, -1, true))
  end)

  it("takes 'eol' and NUL bytes from the file", function()
    edit('a\n')

    reload('a\0b\nc')

    eq({ 'a\0b', 'c' }, api.nvim_buf_get_lines(0, 0, -1, true))
    eq(false, api.nvim_get_option_value('eol', {}))
  end)

  it('keeps diagnostics on their text until they are replaced', function()
    edit('a\nb\nc\n')
    local ns = api.nvim_create_namespace('test')
    exec_lua(function()
      vim.diagnostic.set(ns, 0, { { lnum = 2, col = 0, message = 'on c' } })
    end)

    reload('new\na\nb\nc\n')

    eq(
      3,
      exec_lua(function()
        return (require('vim.diagnostic._shared').get_logical_pos(vim.diagnostic.get(0)[1]))
      end)
    )
  end)
end)
