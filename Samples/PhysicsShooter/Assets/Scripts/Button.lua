-- A button the player presses with E (the Interact action) from within `reach` meters, while the game is being played.
-- Pressed, it sinks by `travel`, gives its points and seconds, shows its message, and calls press() on the script of
-- its `target`, such as a gate. A button that is not `once` comes back out after half a second and can be pressed again.
local Button = {
  properties = {
    target = Entity(),
    reach = 2,
    points = 0,
    time = 0,
    message = "",
    hint = "Press E",
    once = true,
    travel = Vec3(0, 0, 0),
  },
}

local function whole(x)
  return x >= 0 and math.floor(x) or math.ceil(x)
end

function Button:start()
  self.rest = self.entity.position
  self.presses = 0
  self.near = false
  self.down = 0 -- Seconds left before it comes back out.
end

function Button:usable()
  return not (self.once and self.presses > 0) and self.down <= 0
end

function Button:in_reach()
  local player = engine.game.player()
  return player ~= nil and (player.world_position - self.entity.world_position):length() <= self.reach
end

function Button:press()
  self.presses = self.presses + 1
  self.down = self.once and math.huge or 0.5
  self.entity.position = self.rest + self.travel
  local points = whole(self.points)
  if points ~= 0 then
    engine.game.add_score(points, self.message)
  elseif self.message ~= "" then
    engine.game.announce(self.message)
  end
  if self.time ~= 0 then
    engine.game.add_time(self.time)
  end
  local other = self.target.valid and self.target.script
  if other and other.press then
    other:press(self.entity)
  end
end

function Button:tick(dt)
  if not engine.game.active() then
    return
  end
  if self.down > 0 and self.down ~= math.huge then
    self.down = self.down - dt
    if self.down <= 0 then
      self.entity.position = self.rest
    end
  end
  local near = engine.game.phase() == "playing" and self:in_reach()
  if near and not self.near and self:usable() and self.hint ~= "" then
    engine.game.announce(self.hint)
  end
  self.near = near
  if near and self:usable() and engine.input.pressed("Interact") then
    self:press()
  end
end

return Button
