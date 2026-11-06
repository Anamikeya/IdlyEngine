-- A gate that opens once every target standing in its room has fallen, or when a button presses it. It slides by
-- `open_offset` at `speed` meters a second and stays open. Give it a kinematic body, so that it pushes what is in its
-- way. The room is a box in the world, from `room_min` to `room_max`; with an empty box only a button opens it.
local Gate = {
  properties = {
    room_min = Vec3(0, 0, 0),
    room_max = Vec3(0, 0, 0),
    open_offset = Vec3(0, 3, 0),
    speed = 1.5,
    message = "The gate opens",
  },
}

local function inside(p, low, high)
  return p.x >= low.x and p.x <= high.x and p.y >= low.y and p.y <= high.y and p.z >= low.z and p.z <= high.z
end

function Gate:start()
  self.closed_at = self.entity.position
  self.opened = false
  self.moved = 0 -- Meters along open_offset.
end

function Gate:has_room()
  local low, high = self.room_min, self.room_max
  return high.x > low.x and high.y > low.y and high.z > low.z
end

-- How many targets still stand in the room.
function Gate:standing()
  local count = 0
  for _, target in ipairs(engine.game.targets()) do
    if inside(target.world_position, self.room_min, self.room_max) then
      count = count + 1
    end
  end
  return count
end

function Gate:open()
  if self.opened then
    return
  end
  self.opened = true
  if self.message ~= "" then
    engine.game.announce(self.message)
  end
end

-- What a button calls.
function Gate:press()
  self:open()
end

function Gate:tick(dt)
  if not self.opened and self:has_room() and engine.game.active() and self:standing() == 0 then
    self:open()
  end
  local distance = self.open_offset:length()
  if not self.opened or self.moved >= distance then
    return
  end
  self.moved = math.min(self.moved + math.max(self.speed, 0) * dt, distance)
  self.entity.position = self.closed_at + self.open_offset * (self.moved / distance)
end

return Gate
