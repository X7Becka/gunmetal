-- trace_band.lua -- find what writes the cockpit band (rows 122..199) of the
-- Gunmetal screen buffer.
--
-- HOW TO RUN (the game must already be in a loaded level, not a menu):
--   Cheat Engine -> Table -> "Show Cheat Table Lua Script"
--   -> File -> Open -> this file -> Execute
--
-- What it does:
--   1. opens gundx.exe;
--   2. reads the screen buffer pointer (0x5a45ec), width (0x5a4618) and
--      height (0x83d630) -- docs/HUD_AUDIT.md section 2.5;
--   3. puts a hardware *write* breakpoint on the first byte of the band
--      (row 122) of that buffer, in both the 8-bit and the 16-bit layout;
--   4. on every hit prints EIP, the EBP chain and the stack words that look
--      like return addresses into gundx.exe;
--   5. writes everything to %USERPROFILE%\gm_trace_band.txt and stops after
--      8 hits, so the game keeps running.
--
-- If the Lua breakpoint fails, the script prints the band addresses and the
-- GUI way to do the same: Memory View -> Ctrl+G -> the 8bpp address ->
-- right click -> "Find out what writes to this address".

local SCREEN_PTR = 0x5A45EC   -- dword: pointer to the indexed screen
local WIDTH_PTR  = 0x5A4618   -- dword: bytes per row
local HEIGHT_PTR = 0x83D630   -- dword: screen height
local BAND_TOP   = 122        -- first row of the cockpit band
local MAX_HITS   = 8
local CODE_LO, CODE_HI = 0x401000, 0x4C0000   -- gundx.exe .text range

local home = os.getenv("USERPROFILE") or "."
local logpath = home .. "\\gm_trace_band.txt"
local out = io.open(logpath, "w")

local function log(s)
  print(s)
  if out then out:write(s .. "\n"); out:flush() end
end

openProcess("gundx.exe")
if getOpenedProcessID() == 0 then
  log("ERROR: gundx.exe is not running (start the game first)")
  return
end

local screen = readInteger(SCREEN_PTR)
local width  = readInteger(WIDTH_PTR)
local height = readInteger(HEIGHT_PTR)
if not screen or not width or screen == 0 or width == 0 then
  log("ERROR: screen buffer is not initialised yet (load a level, then rerun)")
  return
end

local addr8  = screen + BAND_TOP * width
local addr16 = screen + BAND_TOP * width * 2
log(string.format("screen=0x%X width=%d height=%d", screen, width, height))
log(string.format("band row %d: 8bpp 0x%X, 16bpp 0x%X",
                  BAND_TOP, addr8, addr16))

local ok, err = pcall(function()
  if not debug_isDebugging() then debugProcess() end
end)
if not ok or not debug_isDebugging() then
  log("ERROR: debugger did not start: " .. tostring(err or "unknown") ..
      " (try Cheat Engine as administrator)")
  log("manual: Memory View -> Ctrl+G -> 0x" .. string.format("%X", addr8) ..
      " -> right click -> Find out what writes to this address")
  return
end

local hits = 0

function debugger_onBreakpoint()
  hits = hits + 1
  log(string.format("HIT %d  EIP=0x%X ESP=0x%X EBP=0x%X",
                    hits, EIP, ESP, EBP))
  local ebp = EBP
  local chain = {}
  for _ = 1, 12 do
    local ret = readInteger(ebp + 4)
    local nextbp = readInteger(ebp)
    if not ret or ret == 0 then break end
    chain[#chain + 1] = string.format("0x%X", ret)
    if not nextbp or nextbp <= ebp then break end
    ebp = nextbp
  end
  if #chain > 0 then log("  ebp-chain: " .. table.concat(chain, " ")) end
  local stack = {}
  for i = 0, 63 do
    local v = readInteger(ESP + i * 4)
    if v and v >= CODE_LO and v < CODE_HI then
      stack[#stack + 1] = string.format("+%02X:0x%X", i * 4, v)
    end
  end
  if #stack > 0 then log("  stack-code: " .. table.concat(stack, " ")) end

  if hits >= MAX_HITS then
    pcall(debug_removeBreakpoint, addr8)
    pcall(debug_removeBreakpoint, addr16)
    log("done: breakpoints removed")
  end
  debug_continueFromBreakpoint(co_run)
  return 1
end

local aok8, aerr8 = pcall(debug_setBreakpoint, addr8, 4, bptWrite)
log("breakpoint 8bpp: " .. (aok8 and "set" or ("FAILED: " .. tostring(aerr8))))
local aok16, aerr16 = pcall(debug_setBreakpoint, addr16, 4, bptWrite)
log("breakpoint 16bpp: " .. (aok16 and "set" or ("FAILED: " .. tostring(aerr16))))
log("now return to the game; the log fills up by itself")
