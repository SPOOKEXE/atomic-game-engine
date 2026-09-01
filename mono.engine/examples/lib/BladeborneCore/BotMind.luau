--!strict

local Config = require(script.Parent.CombatConfig)

local BotMind = {}

export type Decision = {
	MoveDirection: Vector3,
	Attack: boolean,
}

-- One deterministic mind. The same bot id and simulation time always produce
-- the same choice, so recordings and load runs agree without a random stream.
function BotMind.decide(botId: number, at: Vector3, target: Vector3?, now: number): Decision
	if target == nil then
		local angle = now * 0.35 + botId * 0.73
		return {
			MoveDirection = Vector3.new(math.cos(angle), 0, math.sin(angle)),
			Attack = false,
		}
	end

	local offset = target - at
	local distance = offset.Magnitude
	if distance <= 0 then
		local angle = botId * 2.399963229728653
		return { MoveDirection = Vector3.new(math.cos(angle), 0, math.sin(angle)), Attack = false }
	end

	local attackBeat = math.floor(now * 5 + botId * 3) % 5 == 0
	return {
		MoveDirection = if distance > Config.Reach * 0.7 then offset.Unit else Vector3.new(0, 0, 0),
		Attack = distance <= Config.Reach and attackBeat,
	}
end

return BotMind
