local t = require('test.testutil')
local n = require('test.functional.testnvim')()

local describe, it, before_each, after_each = t.describe, t.it, t.before_each, t.after_each
local clear = n.clear
local command = n.command
local eq = t.eq
local fn = n.fn
local rmdir = n.rmdir
local mkdir = t.mkdir
local pcall_err = t.pcall_err
local write_file = t.write_file

describe(':file', function()
  local swapdir = vim.uv.cwd() .. '/Xtest-file_spec'
  before_each(function()
    clear()
    rmdir(swapdir)
    mkdir(swapdir)
  end)
  after_each(function()
    command('%bwipeout!')
    rmdir(swapdir)
  end)

  it('rename does not lose swapfile #6487', function()
    local testfile = 'test-file_spec'
    local testfile_renamed = testfile .. '-renamed'
    -- Note: `set swapfile` *must* go after `set directory`: otherwise it may
    -- attempt to create a swapfile in different directory.
    command('set directory^=' .. swapdir .. '//')
    command('set swapfile fileformat=unix undolevels=-1')

    command('edit! ' .. testfile)
    -- Before #6487 this gave "E301: Oops, lost the swap file !!!" on Windows.
    command('file ' .. testfile_renamed)
    eq(testfile_renamed .. '.swp', string.match(fn.execute('swapname'), '[^%%]+$'))
  end)

  it('does not mark the buffer not-edited when renamed to the same file', function()
    local old = t.tmpname()
    local new = old .. '-renamed'
    t.finally(function()
      os.remove(new)
    end)
    write_file(old, 'original\n')
    command('edit ' .. old)
    assert(vim.uv.fs_rename(old, new))
    command('file ' .. new)
    eq(nil, fn.execute('file'):find('[Not edited]', 1, true))
    command('write')
    eq('original\n', t.read_file(new))
  end)

  it('marks the buffer not-edited when renamed to another file', function()
    local path = t.tmpname()
    local other = t.tmpname()
    write_file(path, 'original\n')
    write_file(other, 'other\n')
    command('edit ' .. path)
    command('file ' .. other)
    t.matches('%[Not edited%]', fn.execute('file'))
    eq('Vim(write):E13: File exists (add ! to override)', pcall_err(command, 'write'))
  end)

  it('tracks the file of a reloaded buffer when renamed', function()
    local old = t.tmpname()
    local new = old .. '-renamed'
    t.finally(function()
      os.remove(new)
    end)
    write_file(old, 'original\n')
    command('edit ' .. old)
    -- Replace the file with a new one, like editors that save atomically.
    write_file(old .. '.tmp', 'replaced\n')
    assert(vim.uv.fs_rename(old .. '.tmp', old))
    command('edit')
    assert(vim.uv.fs_rename(old, new))
    command('file ' .. new)
    command('write')
    eq('replaced\n', t.read_file(new))
  end)
end)
