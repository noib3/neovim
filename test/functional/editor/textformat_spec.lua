local t = require('test.testutil')
local n = require('test.functional.testnvim')()

local describe, it, before_each = t.describe, t.it, t.before_each
local clear = n.clear
local command = n.command
local eq = t.eq
local exec_lua = n.exec_lua
local feed = n.feed
local api = n.api

describe('formatting', function()
  before_each(function()
    clear()
    command('set textwidth=24')
  end)

  it('ignores inline virtual text', function()
    api.nvim_buf_set_lines(0, 0, -1, true, { 'The quick brown fox', 'jumps' })
    exec_lua(function()
      local ns = vim.api.nvim_create_namespace('test')
      vim.api.nvim_buf_set_extmark(0, ns, 0, 3, {
        virt_text = { { ': Type', 'Comment' } },
        virt_text_pos = 'inline',
      })
    end)
    feed('gqip')
    eq({ 'The quick brown fox', 'jumps' }, api.nvim_buf_get_lines(0, 0, -1, true))
    feed('ggA bar<Esc>')
    eq({ 'The quick brown fox bar', 'jumps' }, api.nvim_buf_get_lines(0, 0, -1, true))
  end)

  describe('with gq', function()
    local function setup(lines)
      api.nvim_buf_set_lines(0, 0, -1, true, lines)
      command('set nomodified')
      exec_lua(function()
        _G.events = {}
        vim.api.nvim_buf_attach(0, false, {
          on_bytes = function(_, _, _, start_row, _, _, old_row, _, _, new_row)
            table.insert(_G.events, { start_row, old_row, new_row })
          end,
        })
      end)
    end

    it("doesn't change wrapped text", function()
      local lines = { 'The quick brown fox', 'jumps over the lazy dog', 'and keeps on running.' }
      setup(lines)
      local tick = api.nvim_buf_get_var(0, 'changedtick')
      local seq = n.fn.undotree().seq_last
      feed('gqip')
      eq(lines, api.nvim_buf_get_lines(0, 0, -1, true))
      eq({}, exec_lua('return _G.events'))
      eq(tick, api.nvim_buf_get_var(0, 'changedtick'))
      eq(false, api.nvim_get_option_value('modified', {}))
      eq(seq, n.fn.undotree().seq_last)
    end)

    it("doesn't change wrapped block comments", function()
      command('set autoindent cindent comments=s1:/*,mb:*,ex:*/')
      local lines = { '  /* The quick brown fox', '   * jumps over the lazy', '   * dog. */' }
      setup(lines)
      feed('gqip')
      eq(lines, api.nvim_buf_get_lines(0, 0, -1, true))
      eq({}, exec_lua('return _G.events'))
    end)

    it("doesn't move extmarks at line breaks that stay", function()
      setup({ 'The quick brown fox', 'jumps over the lazy dog', 'and keeps on running.' })
      local ns = api.nvim_create_namespace('test')
      local eol = api.nvim_buf_set_extmark(0, ns, 0, 19, {})
      local bol = api.nvim_buf_set_extmark(0, ns, 1, 0, { right_gravity = false })
      feed('gqip')
      eq({ 0, 19 }, api.nvim_buf_get_extmark_by_id(0, ns, eol, {}))
      eq({ 1, 0 }, api.nvim_buf_get_extmark_by_id(0, ns, bol, {}))
    end)

    it('only changes the lines whose wrapping changes', function()
      setup({
        'The quick brown fox',
        'jumps over the lazy dog',
        'and keeps on running and running.',
      })
      feed('gqip')
      eq({
        'The quick brown fox',
        'jumps over the lazy dog',
        'and keeps on running and',
        'running.',
      }, api.nvim_buf_get_lines(0, 0, -1, true))
      eq({ { 2, 0, 1 } }, exec_lua('return _G.events'))
    end)

    it('moves extmarks with the words that move to another line', function()
      setup({ 'The quick fox', 'jumps over the lazy dog' })
      local ns = api.nvim_create_namespace('test')
      local over = api.nvim_buf_set_extmark(0, ns, 1, 6, { end_row = 1, end_col = 10 })
      local the = api.nvim_buf_set_extmark(0, ns, 1, 11, {})
      feed('gqip')
      eq({ 'The quick fox jumps over', 'the lazy dog' }, api.nvim_buf_get_lines(0, 0, -1, true))
      eq({
        0,
        20,
        { end_row = 0, end_col = 24, ns_id = ns, right_gravity = true, end_right_gravity = false },
      }, api.nvim_buf_get_extmark_by_id(0, ns, over, { details = true }))
      eq({ 1, 0 }, api.nvim_buf_get_extmark_by_id(0, ns, the, {}))
    end)
  end)
end)
