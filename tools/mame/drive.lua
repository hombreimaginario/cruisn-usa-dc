-- Conduce en MAME con un guion fijo y saca capturas, para comparar con el port.
-- Variables: DRV_START (frame en que acelera y gira a la izquierda),
--   SNAP_FROM, SNAP_TO, SNAP_EVERY, END_FRAME. Usar con -snapshot_directory.
local f=0
local P=manager.machine.ioport.ports
local function fld(p,n) return P[p].fields[n] end
local coin=fld(":IN0","Coin 1"); local start=fld(":IN0","1 Player Start")
local wheel=fld(":WHEEL","Paddle"); local gas=fld(":ACCEL","P1 Pedal 1")
local D0=tonumber(os.getenv("DRV_START") or "99999")
local SN0=tonumber(os.getenv("SNAP_FROM") or "1500")
local SN1=tonumber(os.getenv("SNAP_TO") or "4500")
local SNE=tonumber(os.getenv("SNAP_EVERY") or "50")
local ENDF=tonumber(os.getenv("END_FRAME") or "4600")
emu.register_frame_done(function()
  f=f+1
  local c=(f==1200 or f==1220 or f==1240) and 1 or 0
  coin:set_value(c)
  local s=(f>=1300 and f<=4000 and (f-1300)%200<5) and 1 or 0
  start:set_value(s)
  if f>=D0 then
    gas:set_value(0x70)
    if f<D0+70 then wheel:set_value(0x40) else wheel:set_value(0x80) end
  else
    gas:set_value(0); wheel:set_value(0x80)
  end
  if f>=SN0 and f<=SN1 and (f-SN0)%SNE==0 then manager.machine.video:snapshot() end
  if f>=ENDF then manager.machine:exit() end
end)
