--!strict

local Config = require(script.Parent.CombatConfig)

local CombatSolver = {}

export type Actor = {
	Id: number,
	Position: Vector3,
	Faction: string,
	Alive: boolean,
}

export type State = {
	Step: number,
	ReadyAt: number,
	ActiveAt: number,
	ComboExpiresAt: number,
	ImpactPending: boolean,
	Buffered: boolean,
}

export type Impact = {
	Step: number,
	Animation: string,
	Damage: number,
}

function CombatSolver.newState(): State
	return {
		Step = 0,
		ReadyAt = 0,
		ActiveAt = 0,
		ComboExpiresAt = 0,
		ImpactPending = false,
		Buffered = false,
	}
end

local function beginAttack(state: State, now: number)
	local continues = state.Step > 0 and state.Step < #Config.Steps and now <= state.ComboExpiresAt
	state.Step = if continues then state.Step + 1 else 1

	local step = Config.Steps[state.Step]
	state.ActiveAt = now + step.Windup
	state.ReadyAt = state.ActiveAt + step.Active + step.Recovery
	state.ComboExpiresAt = now + Config.ComboResetWindow
	state.ImpactPending = true
	state.Buffered = false
end

function CombatSolver.tryAttack(state: State, now: number): (boolean, string)
	if now < state.ReadyAt then
		if state.ReadyAt - now <= Config.InputBuffer then
			state.Buffered = true
			return true, "buffered"
		end
		return false, "recovery"
	end

	beginAttack(state, now)
	return true, "started"
end

function CombatSolver.advance(state: State, now: number): Impact?
	local impact = nil
	if state.ImpactPending and now >= state.ActiveAt then
		local step = Config.Steps[state.Step]
		impact = {
			Step = state.Step,
			Animation = step.Animation,
			Damage = Config.BaseDamage * step.DamageMultiplier,
		}
		state.ImpactPending = false
	end

	if state.Buffered and now >= state.ReadyAt then
		beginAttack(state, now)
	end

	return impact
end

function CombatSolver.nearestTarget(
	origin: Vector3,
	forward: Vector3,
	faction: string,
	actors: { Actor }
): Actor?
	if forward.Magnitude <= 0 then
		return nil
	end

	local direction = forward.Unit
	local nearest = nil
	local nearestDistance = math.huge
	for _, actor in actors do
		if actor.Alive and actor.Faction ~= faction then
			local offset = actor.Position - origin
			local distance = offset.Magnitude
			local inReach = distance > 0 and distance <= Config.Reach
			local inArc = inReach and offset:Dot(direction) / distance >= Config.MinimumForwardDot
			if inArc then
				local winsTie = distance == nearestDistance and nearest ~= nil and actor.Id < nearest.Id
				if distance < nearestDistance or winsTie then
					nearest = actor
					nearestDistance = distance
				end
			end
		end
	end
	return nearest
end

return CombatSolver
