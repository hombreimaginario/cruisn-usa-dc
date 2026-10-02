local cpu = manager.machine.devices[":maincpu"]
local sp = cpu.spaces["program"]
local hits = 0
local target = tonumber(os.getenv("BP"), 16)
local maxhits = tonumber(os.getenv("BPN") or "20")
cpu.debug:bpset(target, "", "")
emu.register_periodic(function() end)
local function regs()
  local s = string.format("pc=%06X", cpu.state["PC"].value)
  for _, r in ipairs({"R0","R1","R2","R3","R4","R5","R6","R7","AR0","AR1","AR2","AR3","AR4","AR5","AR6","AR7","DP","IR0","IR1","BK","SP","ST","IE","IF","RS","RE","RC"}) do
    s = s .. string.format(" %s=%08X", r, cpu.state[r].value & 0xffffffff)
  end
  return s
end
emu.add_machine_stop_notifier(function() end)
local dbg = manager.machine.debugger
emu.register_frame_done(function() end)
-- poll: when stopped at bp, log and continue
emu.register_periodic(function()
  if dbg.execution_state == "stop" then
    hits = hits + 1
    print(regs())
    if hits >= maxhits then manager.machine:exit() end
    dbg.execution_state = "run"
  end
end)
