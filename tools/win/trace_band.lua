-- trace_band.lua -- find what writes the cockpit band of the Gunmetal screen.
--
-- v2 (2026-10-23): the live screen is NOT 320x200 -- the globals report the
-- real surface (the first run printed width=800 height=600), so the band top
-- is computed as `height * 122 / 200`, i.e. row 366 at 800x600.  The first
-- run watched row 122, which is inside the world, and caught the model pass
-- (stack ... 0x4469C1 -> 0x447FB2).  This version watches three rows of the
-- band, names the writing module, and keeps a deduplicated log.
--
-- HOW TO RUN (game already in a loaded level, not a menu):
--   Cheat Engine -> Table -> "Show Cheat Table Lua Script"
--   -> File -> Open -> this file -> Execute
--
-- Log: %USERPROFILE%\gm_trace_band.txt

local SCREEN_PTR = 0x5A45EC   -- dword: pointer to the indexed screen
local WIDTH_PTR  = 0x5A4618   -- dword: bytes per row
local HEIGHT_PTR = 0x83D630   -- dword: screen height
local BAND_TOP_200 = 122      -- band top in the 320x200 reference frame
local MAX_HITS   = 60
local CODE_LO, CODE_HI = 0x401000, 0x4C0000   -- gundx.exe .text range

local home = os.getenv("USERPROFILE") or "."
local out = io.open(home .. "\\gm_trace_band.txt", "w")

local function log(s)
  print(s)
  if out then out:write(s .. "\n"); out:flush() end
end

openProcess("gundx.exe")
if getOpenedProcessID() == 0 then
  log("ERROR: gundx.exe is not running")
  return
end

local screen = readInteger(SCREEN_PTR)
local width  = readInteger(WIDTH_PTR)
local height = readInteger(HEIGHT_PTR)
if not screen or not width or screen == 0 or width == 0 or height == 0 then
  log("ERROR: screen buffer is not initialised (load a level, then rerun)")
  return
end
log(string.format("screen=0x%X width=%d height=%d", screen, width, height))

-- three probe rows inside the band: its top, the middle and near the bottom
local top = math.floor(height * BAND_TOP_200 / 200)
local rows = { top, math.floor((top + height) / 2), height - 8 }
local addrs = {}
for _, r in ipairs(rows) do
  addrs[#addrs + 1] = { row = r, addr = screen + r * width }
  log(string.format("watch row %d -> 0x%X", r, screen + r * width))
end

local ok, err = pcall(function()
  if not debug_isDebugging() then debugProcess() end
end)
if not ok or not debug_isDebugging() then
  log("ERROR: debugger did not start: " .. tostring(err or "unknown"))
  log("manual: Memory View -> Ctrl+G -> the 8bpp address above -> " ..
      "Find out what writes to this address")
  return
end

local hits = 0
local seen = {}

function debugger_onBreakpoint()
  hits = hits + 1
  local stack = {}
  for i = 0, 95 do
    local v = readInteger(ESP + i * 4)
    if v and v >= CODE_LO and v < CODE_HI then
      stack[#stack + 1] = string.format("%X", v)
    end
  end
  local sig = string.format("%X|%s", EIP, table.concat(stack, ","))
  if seen[sig] then
    if hits < MAX_HITS then debug_continueFromBreakpoint(co_run); return 1 end
  end
  seen[sig] = true
  log(string.format("HIT %d  EIP=0x%X (%s)  ESP=0x%X  EBP=0x%X",
                    hits, EIP, tostring(getNameFromAddress(EIP)), ESP, EBP))
  log("  stack-code: " .. table.concat(stack, " "))
  if hits >= MAX_HITS then
    for _, a in ipairs(addrs) do pcall(debug_removeBreakpoint, a.addr) end
    log("done: breakpoints removed")
  end
  debug_continueFromBreakpoint(co_run)
  return 1
end

for _, a in ipairs(addrs) do
  local aok, aerr = pcall(debug_setBreakpoint, a.addr, 4, bptWrite)
  log(string.format("breakpoint row %d: %s", a.row,
                    aok and "set" or ("FAILED: " .. tostring(aerr))))
end
log("now play for ~5 seconds; the log fills up by itself")
