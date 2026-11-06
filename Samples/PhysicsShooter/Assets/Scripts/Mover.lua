-- Moves its entity back and forth between two points in the world at a steady speed: the mover behavior, as a script.
-- Give the entity a kinematic body, so that what it carries is moved by physics. It plays tick for tick the same as
-- the C++ behavior with the same values (Tests/Gameplay/scripted_behavior_tests.cpp).
local Mover = {
  properties = {
    a = Vec3(0, 0, 0),
    b = Vec3(0, 0, 0),
    speed = 2,
  },
}

function Mover:start()
  self.travelled = 0 -- Along the path there and back, in meters.
  if self.entity.dynamic then
    log.warn(self.entity.name .. " has a dynamic body: physics moves it, so a mover needs a kinematic one")
  end
end

function Mover:tick(dt)
  local a, path = self.a, self.b - self.a
  local length = path:length()
  if length <= 0 then
    return
  end
  self.travelled = math.fmod(self.travelled + math.max(self.speed, 0) * dt, 2 * length)
  local along = (self.travelled <= length and self.travelled or 2 * length - self.travelled) / length
  self.entity.position = a + path * along
end

return Mover
