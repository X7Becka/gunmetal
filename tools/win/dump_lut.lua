-- dump_lut.lua -- dump the three runtime artefacts the cockpit band needs.
--
-- Run in Cheat Engine while gundx.exe is in a loaded level (as for
-- trace_band.lua).  Writes three files next to %USERPROFILE%:
--
--   gm_lut.bin      4096 bytes at 0x682f90 -- 16 fade LUTs of 256 bytes,
--                   built at startup by 0x48f970 -> 0x483290 and applied by
--                   the band blit 0x00a1b510 (source index -> screen index).
--   gm_panel.bin    24960 bytes at 0x678188 -- the loaded "panel" image
--                   (320x78), filled by 0x485890 via 0x47efe0 from
--                   FRONTEND/<name>.  This is what the engine really blits.
--   gm_band.bin     the engine's own band, 234 rows x 800 px (8bpp) from
--                   screen+366*800 -- no dgVoodoo scaling, the raw surface.
--   gm_screen.bin   the whole screen surface (800x600, 8bpp).
--   gm_hud75.bin    the HUD intermediate 0x6c3160 (320x75) and
--   gm_hud16.bin    0x6c3ca0 (320x16), where the layer composes before scaling.
--
-- Everything is read-only.

local SCREEN_PTR = 0x5A45EC
local WIDTH_PTR  = 0x5A4618
local HEIGHT_PTR = 0x83D630
local LUT_ADDR   = 0x682F90
local LUT_SIZE   = 16 * 256
local PANEL_ADDR = 0x678188
local PANEL_SIZE = 320 * 78
local BAND_TOP_200 = 122

local home = os.getenv("USERPROFILE") or "."

local function dump(path, addr, size)
  local f = io.open(path, "wb")
  if not f then print("cannot write " .. path) return end
  local chunk = {}
  for i = 0, size - 1 do
    chunk[#chunk + 1] = string.char(readByte(addr + i) or 0)
    if #chunk >= 4096 then f:write(table.concat(chunk)); chunk = {} end
  end
  if #chunk > 0 then f:write(table.concat(chunk)) end
  f:close()
  print(string.format("%s: 0x%X..0x%X (%d bytes)", path, addr, addr + size - 1, size))
end

openProcess("gundx.exe")
if getOpenedProcessID() == 0 then
  print("ERROR: gundx.exe is not running")
  return
end

dump(home .. "\\gm_lut.bin", LUT_ADDR, LUT_SIZE)
dump(home .. "\\gm_panel.bin", PANEL_ADDR, PANEL_SIZE)
-- HUD intermediates: the layer composes 320x75 here (0x49cb70 clears 75 rows)
-- and blits 9 + 16 rows from the second one (0x6c3ca0) to the screen.
dump(home .. "\\gm_hud75.bin", 0x6C3160, 320 * 75)
dump(home .. "\\gm_hud16.bin", 0x6C3CA0, 320 * 16)

local screen = readInteger(SCREEN_PTR)
local width  = readInteger(WIDTH_PTR)
local height = readInteger(HEIGHT_PTR)
if screen and width and height and screen ~= 0 and width ~= 0 then
  local top = math.floor(height * BAND_TOP_200 / 200)
  dump(home .. "\\gm_band.bin", screen + top * width, (height - top) * width)
  dump(home .. "\\gm_screen.bin", screen, width * height)
  print(string.format("screen=0x%X %dx%d, band rows %d..%d",
                      screen, width, height, top, height - 1))
else
  print("screen buffer is not initialised -- start a level and rerun")
end
