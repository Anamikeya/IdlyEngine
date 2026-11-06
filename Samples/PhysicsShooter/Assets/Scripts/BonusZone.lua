-- A sensor that scores once for each body physics moves that falls into it, not counting the player: the bonus_zone
-- behavior, as a script. It plays the same as the C++ one with the same points.
local BonusZone = {
  properties = {
    points = 50,
  },
}

local function whole(x)
  return x >= 0 and math.floor(x) or math.ceil(x)
end

function BonusZone:start()
  self.scored = {} -- Entity IDs already counted.
end

function BonusZone:on_trigger_enter(other)
  if not other.dynamic or other == engine.game.player() or self.scored[other.id] then
    return
  end
  self.scored[other.id] = true
  local points = whole(self.points)
  engine.game.add_score(points, string.format("+%d  Bonus", points))
end

return BonusZone
