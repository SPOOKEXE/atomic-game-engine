--!strict

local RunService = game:GetService("RunService")
local UserInputService = game:GetService("UserInputService")
local Players = game:GetService("Players")

local Modules = script.Parent
local CombatSolver = require(Modules.BladeborneCore.CombatSolver)
local BotMind = require(Modules.BladeborneCore.BotMind)
local CombatConfig = require(Modules.BladeborneCore.CombatConfig)
local SpatialIndex = require(Modules.BladeborneCore.SpatialIndex)
local ProfileStore = require(Modules.BladeborneRuntime.ProfileStore)
local Hud = require(Modules.BladeborneRuntime.Hud)

local BladeborneRuntime = {}

export type StartOptions = {
	MindAllPlayers: boolean?,
	ProfilesEnabled: boolean?,
	BindInput: boolean?,
	HudEnabled: boolean?,
}

type CombatState = CombatSolver.State
type Actor = CombatSolver.Actor

type RuntimeState = {
	NextActorId: number,
	Now: number,
	AttackQueued: boolean,
	States: { [number]: CombatState },
	Profiles: ProfileStore.Controller,
	MindAllPlayers: boolean,
	Connections: { RBXScriptConnection },
}

local function rootOf(character: Instance): BasePart?
	return character:FindFirstChild("HumanoidRootPart") :: BasePart?
end

local function humanoidOf(character: Instance): Humanoid?
	return character:FindFirstChild("Humanoid") :: Humanoid?
end

local function characterOf(player: Instance): Instance?
	local character = player.Character
	if character == nil then
		return nil
	end
	return character
end

local function actorId(state: RuntimeState, character: Instance): number
	local existing = character:GetAttribute("BladeborneActorId")
	if typeof(existing) == "number" then
		return existing
	end

	local id = state.NextActorId
	state.NextActorId += 1
	character:SetAttribute("BladeborneActorId", id)
	return id
end

local function combatState(state: RuntimeState, character: Instance): CombatState
	local id = actorId(state, character)
	local existing = state.States[id]
	if existing ~= nil then
		return existing
	end

	local created = CombatSolver.newState()
	state.States[id] = created
	return created
end

local function collectActors(state: RuntimeState): ({ Actor }, { [number]: Instance })
	local actors = {}
	local characters = {}
	local activeIds = {}
	for _, player in Players:GetPlayers() do
		local character = characterOf(player)
		if character == nil then
			continue
		end
		local root = rootOf(character)
		local humanoid = humanoidOf(character)
		if root == nil or humanoid == nil then
			continue
		end

		local id = actorId(state, character)
		activeIds[id] = true
		local faction = player:GetAttribute("BladeborneFaction")
		if typeof(faction) ~= "string" or faction == "" then
			if state.MindAllPlayers then
				faction = if id % 2 == 0 then "husk" else "warden"
			else
				faction = if player:GetAttribute("BladeborneBot") == true then "enemy" else "player"
			end
		end
		characters[id] = character
		table.insert(actors, {
			Id = id,
			Position = character:GetPivot().Position,
			Faction = faction,
			Alive = humanoid.Health > 0,
		})
	end
	for id in state.States do
		if activeIds[id] ~= true then
			state.States[id] = nil
		end
	end
	return actors, characters
end

local function resolveImpact(
	state: RuntimeState,
	actor: Actor,
	character: Instance,
	forward: Vector3,
	spatial: SpatialIndex.Grid,
	characters: { [number]: Instance },
	now: number
)
	local impact = CombatSolver.advance(combatState(state, character), now)
	if impact == nil then
		return
	end
	local target = SpatialIndex.nearestTarget(spatial, actor.Position, forward, actor.Faction, CombatConfig.Reach)
	if target == nil then
		return
	end
	local targetCharacter = characters[target.Id]
	if targetCharacter == nil then
		return
	end
	local humanoid = humanoidOf(targetCharacter)
	if humanoid == nil or humanoid.Health <= 0 then
		return
	end

	local previousHealth = humanoid.Health
	humanoid.Health = math.max(0, previousHealth - impact.Damage)
	targetCharacter:SetAttribute("LastBladeborneHit", now)
	character:SetAttribute("BladeborneComboStep", impact.Step)
	if previousHealth > 0 and humanoid.Health <= 0 then
		local player = Players:GetPlayerFromCharacter(character)
		if player ~= nil then
			state.Profiles.grantKill(player, 35, 12)
			local kills = player:GetAttribute("BladeborneBotKills")
			player:SetAttribute("BladeborneBotKills", if typeof(kills) == "number" then kills + 1 else 1)
		end
	end
end

local function tick(state: RuntimeState, now: number)
	local actors, characters = collectActors(state)
	local spatial = SpatialIndex.build(actors, CombatConfig.Reach * 2)
	local localPlayer = Players.LocalPlayer

	for _, actor in actors do
		local character = characters[actor.Id]
		if character == nil then
			continue
		end
		local root = rootOf(character)
		local humanoid = humanoidOf(character)
		if root == nil or humanoid == nil or not actor.Alive then
			continue
		end

		local player = Players:GetPlayerFromCharacter(character)
		local isBot = player ~= nil and (state.MindAllPlayers or player:GetAttribute("BladeborneBot") == true)
		local attack = false
		local forward = root.CFrame.LookVector

		if isBot then
			local target = SpatialIndex.nearestEnemy(spatial, actor, 72)
			local decision = BotMind.decide(actor.Id, actor.Position, if target == nil then nil else target.Position, now)
			humanoid.MoveDirection = decision.MoveDirection
			attack = decision.Attack
			if target ~= nil then
				local targetDirection = target.Position - actor.Position
				if targetDirection.Magnitude > 0 then
					forward = targetDirection.Unit
				end
			end
		elseif player ~= nil and localPlayer ~= nil and player:Equals(localPlayer) and state.AttackQueued then
			attack = true
			state.AttackQueued = false
		end

		if attack then
			CombatSolver.tryAttack(combatState(state, character), now)
		end
		resolveImpact(state, actor, character, forward, spatial, characters, now)
	end
end

function BladeborneRuntime.start(options: StartOptions?): () -> ()
	local selected = options or {}
	local localPlayer = Players.LocalPlayer
	local showHud = selected.HudEnabled ~= false
	local stopHud = if localPlayer == nil or not showHud then function() end else Hud.start(localPlayer)
	local state: RuntimeState = {
		NextActorId = 1,
		Now = 0,
		AttackQueued = false,
		States = {},
		Profiles = ProfileStore.start(selected.ProfilesEnabled),
		MindAllPlayers = selected.MindAllPlayers == true,
		Connections = {},
	}

	if selected.BindInput ~= false then
		table.insert(state.Connections, UserInputService.InputBegan:Connect(function(input, processed)
			if not processed and input.UserInputType == Enum.UserInputType.MouseButton1 then
				state.AttackQueued = true
			end
		end))
	end
	table.insert(state.Connections, RunService.Heartbeat:Connect(function(deltaTime)
		state.Now += deltaTime
		tick(state, state.Now)
	end))

	return function()
		for _, connection in state.Connections do
			connection:Disconnect()
		end
		table.clear(state.Connections)
		state.Profiles.stop()
		stopHud()
	end
end

return BladeborneRuntime
