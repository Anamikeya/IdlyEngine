-- A sensor the player collects by walking through it: rounds for the weapon, seconds on the clock, then it is gone.
-- The pickup behavior, as a script; it plays the same as the C++ one with the same values.
local Pickup = {
  properties = {
    ammo = 0,
    time = 0,
  },
}

-- A whole number, as C++ makes one from a number: toward zero.
local function whole(x)
  return x >= 0 and math.floor(x) or math.ceil(x)
end

function Pickup:on_trigger_enter(other)
  if self.taken or other ~= engine.game.player() then
    return
  end
  self.taken = true
  engine.game.give_ammo(whole(self.ammo))
  engine.game.add_time(self.time)
  engine.game.announce(self.time > 0 and string.format("+%d seconds", whole(self.time)) or "Ammo collected")
  self.entity:destroy()
end

return Pickup
