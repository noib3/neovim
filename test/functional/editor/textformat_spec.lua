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
end)
