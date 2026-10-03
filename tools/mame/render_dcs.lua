-- Graba la salida de la placa de sonido DCS para una lista de codigos.
--
-- Uso (desde tools/mame/render_dcs.py):
--   mame crusnusa41 ... -wavwrite salida.wav -autoboot_script render_dcs.lua
-- Variables de entorno:
--   DCS_CODES  fichero con "codigo segundos" por linea (codigo en decimal)
--   DCS_START  frame en el que se toma el control (por defecto 900)
--
-- Tras el arranque se parchea SENDSND (0x9211) con RETS para que el juego
-- deje de mandar sonidos, y se envian los codigos uno a uno escribiendo en
-- el puerto 0x9A0000 (un byte por escritura, como hace el juego). Se imprime
-- "CODE <codigo> <segundo de emulacion>" para trocear el WAV despues.

local cpu = manager.machine.devices[":maincpu"]
local sp = cpu.spaces["program"]
local start = tonumber(os.getenv("DCS_START") or "900")
local list = {}
for line in io.lines(os.getenv("DCS_CODES")) do
  local c, s = line:match("(%d+)%s+([%d%.]+)")
  if c then table.insert(list, { code = tonumber(c), secs = tonumber(s) }) end
end

local function now()
  return manager.machine.time:as_double()
end

local function send_byte(b)
  sp:write_u32(0x9A0000, 0xFF00 | (b & 0xFF))
end

local function send(code)
  send_byte(code >> 8)
  emu.wait(0.002)
  send_byte(code & 0xFF)
  emu.wait(0.002)
end

local frame = 0
local started = false
emu.register_frame_done(function()
  frame = frame + 1
  if frame == start and not started then
    started = true
    sp:write_u32(0x9211, 0x78800000)          -- SENDSND: RETS
    local co = coroutine.wrap(function()
      send(0x0000)
      emu.wait(1.0)
      for _, e in ipairs(list) do
        send(0x0000)
        emu.wait(0.2)
        print(string.format("CODE %d %.6f", e.code, now()))
        send(e.code)
        emu.wait(e.secs)
      end
      send(0x0000)
      print(string.format("END %.6f", now()))
      emu.wait(0.5)
      manager.machine:exit()
    end)
    co()
  end
end)
