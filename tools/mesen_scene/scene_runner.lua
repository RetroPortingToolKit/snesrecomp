-- scene_runner.lua -- drive Mesen2 with the snesrecomp scene-script grammar.
--
-- The same script file drives three runners with the same per-frame meaning:
-- the recomp host (--script, runner/src/desktop/host_main.c), snesref
-- (SNESREF_SCRIPT, tools/snesref) and this. Mesen2 is the cycle-accurate one
-- (SuperFX timing, H/V IRQ, HDMA), so it is the oracle for anything timing-
-- shaped; snesref is the one with the PPU write journal.
--
-- Run (headless):
--   Mesen.exe --testrunner <rom> scene_runner.lua
-- Configuration comes from a sidecar file, because --testrunner passes no
-- arguments to the script: <this script's directory>/scene_runner.cfg, or the
-- file named by the SCENE_CFG environment variable, with lines
--   script=<path to scene script>
--   dump_dir=<output directory>
--   watch=0030,0118          (optional: WRAM bytes logged EVERY frame to
--                             <dump_dir>/watch.tsv -- an always-on record,
--                             so a timing question is answered by reading it
--                             back, never by re-running with a probe armed)
--
-- Grammar (see host_main.c LoadScript): wait N | press b[+b..] [N] |
-- poke A HEX | pokefor A HEX N | forcepoke A HEX | until A ==|!= V [T] |
-- until16 ... | dump TAG | quit. Addresses are WRAM offsets ($7E:0118 = 0118).
-- Every hold-type entry is followed by one idle frame; until/dump/quit cost no
-- frames. A timed-out until exits with code 3.
--
-- Dump files: <tag>.fb.png, .wram.bin, .vram.bin, .cgram.bin, .oam.bin,
-- .sram.bin, .state.txt (Mesen's full emu.getState(), sorted), .info.json.

local function read_cfg()
  local path = os.getenv("SCENE_CFG")
  if not path or path == "" then
    local src = debug.getinfo(1, "S").source:gsub("^@", "")
    path = src:gsub("[^/\\]+$", "") .. "scene_runner.cfg"
  end
  local cfg = {}
  local f = io.open(path, "r")
  if not f then emu.log("scene_runner: no cfg at " .. path); emu.stop(2); return cfg end
  for line in f:lines() do
    local k, v = line:match("^%s*([%w_]+)%s*=%s*(.-)%s*$")
    if k then cfg[k] = v end
  end
  f:close()
  return cfg
end

local cfg = read_cfg()
local dump_dir = cfg.dump_dir or "."
local wram = emu.memType.snesWorkRam

local BUTTONS = { b = "b", y = "y", select = "select", start = "start", up = "up",
  down = "down", left = "left", right = "right", a = "a", x = "x", l = "l", r = "r" }

local function parse_buttons(s)
  local t = {}
  for part in s:gmatch("[^+,|]+") do
    local k = BUTTONS[part:lower()]
    if k then t[k] = true else emu.log("scene_runner: unknown button " .. part) end
  end
  return t
end

local function hexbytes(s)
  local t = {}
  for i = 1, #s - 1, 2 do t[#t + 1] = tonumber(s:sub(i, i + 1), 16) end
  return t
end

-- Parse into entries, folding waits into the next entry like LoadScript.
local entries, pending = {}, 0
do
  local f = io.open(cfg.script or "", "r")
  if not f then emu.log("scene_runner: cannot open script " .. tostring(cfg.script)); emu.stop(2) end
  for raw in f:lines() do
    local line = raw:gsub("#.*$", "")
    local w = {}
    for tok in line:gmatch("%S+") do w[#w + 1] = tok end
    local c = w[1]
    local function add(e) e.wait = pending; pending = 0; entries[#entries + 1] = e end
    if c == nil then
    elseif c == "wait" then pending = pending + (tonumber(w[2]) or 0)
    elseif c == "press" then add({ kind = "hold", input = parse_buttons(w[2] or ""), hold = tonumber(w[3]) or 1 })
    elseif c == "poke" then add({ kind = "poke", addr = tonumber(w[2], 16), bytes = hexbytes(w[3] or ""), hold = 1 })
    elseif c == "pokefor" then add({ kind = "poke", addr = tonumber(w[2], 16), bytes = hexbytes(w[3] or ""), hold = math.max(1, tonumber(w[4]) or 1) })
    elseif c == "forcepoke" then add({ kind = "force", addr = tonumber(w[2], 16), bytes = hexbytes(w[3] or ""), hold = 1 })
    elseif c == "until" or c == "until16" then
      add({ kind = "until", addr = tonumber(w[2], 16), ne = (w[3] == "!="), value = tonumber(w[4], 16),
            timeout = tonumber(w[5]) or 36000, width = (c == "until16") and 2 or 1, waited = 0 })
    elseif c == "dump" then add({ kind = "dump", tag = w[2] })
    elseif c == "quit" then add({ kind = "quit" })
    elseif c == "loadstate" or c == "reset" then
      emu.log("scene_runner: '" .. c .. "' not supported; consuming its frame")
      add({ kind = "hold", input = {}, hold = 1 })
    else emu.log("scene_runner: unknown command " .. c) end
  end
  f:close()
end

local forced = {}
local idx, phase, counter = 1, 1, (entries[1] and entries[1].wait or 0)
local frame = 0          -- completed frames
local cur_input = {}
local exit_code = nil

local function write_mem(addr, bytes)
  for i, b in ipairs(bytes) do emu.write(addr + i - 1, b, wram) end
end

local function blob(path, mtype, size)
  local f = io.open(path, "wb")
  local parts = {}
  for i = 0, size - 1 do parts[#parts + 1] = string.char(emu.read(i, mtype)) end
  f:write(table.concat(parts)); f:close()
end

local function dump(tag)
  local base = dump_dir .. "/" .. tag
  local png = emu.takeScreenshot()
  local f = io.open(base .. ".fb.png", "wb"); f:write(png); f:close()
  blob(base .. ".wram.bin", wram, 0x20000)
  blob(base .. ".vram.bin", emu.memType.snesVideoRam, 0x10000)
  blob(base .. ".cgram.bin", emu.memType.snesCgRam, 0x200)
  blob(base .. ".oam.bin", emu.memType.snesSpriteRam, 0x220)
  local ssize = emu.getMemorySize(emu.memType.snesSaveRam)
  if ssize and ssize > 0 then blob(base .. ".sram.bin", emu.memType.snesSaveRam, ssize) end
  local st = emu.getState()
  local keys = {}
  for k in pairs(st) do keys[#keys + 1] = k end
  table.sort(keys)
  f = io.open(base .. ".state.txt", "w")
  for _, k in ipairs(keys) do f:write(k, "\t", tostring(st[k]), "\n") end
  f:close()
  f = io.open(base .. ".info.json", "w")
  f:write(string.format('{"frame": %d, "source": "mesen2", "sram_size": %d}\n', frame, ssize or 0))
  f:close()
  emu.log(string.format("script f=%d dump %s ok", frame, tag))
end

local function cond(e)
  local v = emu.read(e.addr, wram)
  if e.width == 2 then v = v + emu.read(e.addr + 1, wram) * 256 end
  return (v == e.value) ~= e.ne
end

-- One call per frame boundary; mirrors host_main.c TickScript exactly.
local function tick()
  for _, p in ipairs(forced) do write_mem(p.addr, p.bytes) end
  while true do
    local e = entries[idx]
    if not e then return {} end
    if phase == 1 then
      if counter > 0 then counter = counter - 1; return {} end
      phase = 0; counter = e.hold or 0
    end
    if e.kind == "until" or e.kind == "dump" or e.kind == "quit" then
      if e.kind == "until" then
        if not cond(e) then
          e.waited = e.waited + 1
          if e.waited > e.timeout then
            emu.log(string.format("script f=%d until %05X timed out", frame, e.addr)); exit_code = 3
          end
          return {}
        end
        emu.log(string.format("script f=%d until %05X ok after %d frames", frame, e.addr, e.waited))
      elseif e.kind == "dump" then dump(e.tag)
      else emu.log(string.format("script f=%d quit", frame)); exit_code = 0 end
      idx = idx + 1
      if entries[idx] then phase = 1; counter = entries[idx].wait end
      if exit_code then return {} end
    else
      if counter > 0 then
        counter = counter - 1
        if e.kind == "poke" then write_mem(e.addr, e.bytes); return {} end
        if e.kind == "force" then forced[#forced + 1] = e; return {} end
        return e.input
      end
      idx = idx + 1
      if entries[idx] then phase = 1; counter = entries[idx].wait end
      return {}
    end
  end
end

local watch = {}
for a in (cfg.watch or ""):gmatch("[^,%s]+") do watch[#watch + 1] = tonumber(a, 16) end
local watch_f = nil
if #watch > 0 then
  watch_f = io.open(dump_dir .. "/watch.tsv", "w")
  local hdr = { "frame" }
  for _, a in ipairs(watch) do hdr[#hdr + 1] = string.format("%04X", a) end
  watch_f:write(table.concat(hdr, "\t"), "\n")
end

-- Mesen's SNES power-on RAM defaults to random, so a script's first `until`
-- can match garbage at frame 1. Key the first wait on a mode the boot passes
-- through (the YI scripts wait for $03 before $07). Filling WRAM from here is
-- NOT an option: the first startFrame fires after reset has already copied
-- its WRAM routines, and zeroing them hangs the game.
emu.addEventCallback(function()
  if watch_f and frame > 0 then
    local row = { tostring(frame) }
    for _, a in ipairs(watch) do row[#row + 1] = string.format("%02X", emu.read(a, wram)) end
    watch_f:write(table.concat(row, "\t"), "\n")
  end
  cur_input = tick()
  if exit_code then
    if watch_f then watch_f:close() end
    emu.stop(exit_code)
  end
end, emu.eventType.startFrame)

emu.addEventCallback(function()
  emu.setInput(cur_input, 0)
end, emu.eventType.inputPolled)

-- gsu_log=1: one line per SuperFX job to <dump_dir>/gsu.tsv -- the master
-- clock of the S-CPU's R15 high-byte write that starts it, and of the first
-- S-CPU read of SFR ($3030) that sees G clear. The oracle half of the recomp's
-- `<tag>.gsu.tsv` (runner/src/snes/superfx.c job ring).
if cfg.gsu_log == "1" then
  local gsu_f = io.open(dump_dir .. "/gsu.tsv", "w")
  gsu_f:write("# start_master\tcpu_seen_stop_master\tclocks\tpc24\tframe\n")
  local open_start, open_pc = nil, 0
  local function on_start(addr, value)
    local st = emu.getState()
    open_start = st["masterClock"]
    open_pc = (st["cart.coprocessor.programBank"] or 0) * 65536 + value * 256 +
              (st["cart.coprocessor.r15"] or 0) % 256
    return value
  end
  local function on_sfr(addr, value)
    if open_start and (value & 0x20) == 0 then
      local stop = emu.getState()["masterClock"]
      gsu_f:write(string.format("%d\t%d\t%d\t%06X\t%d\n", open_start, stop, stop - open_start, open_pc, frame))
      gsu_f:flush()
      open_start = nil
    end
    return value
  end
  for bank = 0x00, 0x3F do
    for _, base in ipairs({ bank * 0x10000, (bank + 0x80) * 0x10000 }) do
      emu.addMemoryCallback(on_start, emu.callbackType.write, base + 0x301F, base + 0x301F,
                            emu.cpuType.snes, emu.memType.snesMemory)
      emu.addMemoryCallback(on_sfr, emu.callbackType.read, base + 0x3030, base + 0x3030,
                            emu.cpuType.snes, emu.memType.snesMemory)
    end
  end
end

emu.addEventCallback(function() frame = frame + 1 end, emu.eventType.endFrame)
