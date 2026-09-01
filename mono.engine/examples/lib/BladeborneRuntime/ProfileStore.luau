--!strict

local DataStoreService = game:GetService("DataStoreService")
local Players = game:GetService("Players")

local Progression = require(script.Parent.Parent.BladeborneCore.Progression)

local ProfileStore = {}

export type Profile = Progression.Profile

export type Controller = {
	profileOf: (player: Instance) -> Profile?,
	grantKill: (player: Instance, experience: number, gold: number) -> number,
	save: (player: Instance) -> boolean,
	stop: () -> (),
}

local function playerId(player: Instance): number
	return player.UserId
end

local function profileKey(player: Instance): string
	return `bladeborne/profile/{playerId(player)}`
end

local function isBot(player: Instance): boolean
	return player:GetAttribute("BladeborneBot") == true
end

local function publish(player: Instance, profile: Profile)
	player:SetAttribute("BladeborneLevel", profile.Level)
	player:SetAttribute("BladeborneExperience", profile.Experience)
	player:SetAttribute("BladeborneGold", profile.Gold)
	player:SetAttribute("BladeborneKills", profile.Kills)
	player:SetAttribute("BladeborneHealthFlasks", profile.HealthFlasks)
	player:SetAttribute("BladeborneWeapon", profile.EquippedWeapon)
end

function ProfileStore.start(enabled: boolean?): Controller
	if enabled == false then
		return {
			profileOf = function(_player: Instance): Profile?
				return nil
			end,
			grantKill = function(_player: Instance, _experience: number, _gold: number): number
				return 0
			end,
			save = function(_player: Instance): boolean
				return true
			end,
			stop = function() end,
		}
	end

	local profiles: { [number]: Profile } = {}
	local connections: { RBXScriptConnection } = {}
	local stopped = false

	local function load(player: Instance)
		if isBot(player) or stopped then
			return
		end
		local id = playerId(player)
		if profiles[id] ~= nil then
			publish(player, profiles[id])
			return
		end

		local stored, status = DataStoreService:GetAsync(profileKey(player))
		if stopped then
			return
		end
		local profile = if status == "Ok" then Progression.sanitize(stored) else Progression.newProfile()
		profiles[id] = profile
		publish(player, profile)
	end

	local function save(player: Instance): boolean
		if isBot(player) then
			return true
		end
		local profile = profiles[playerId(player)]
		if profile == nil then
			return false
		end
		local _, status = DataStoreService:SetAsync(profileKey(player), profile)
		return status == "Ok"
	end

	for _, player in Players:GetPlayers() do
		task.spawn(load, player)
	end
	table.insert(connections, Players.PlayerAdded:Connect(function(player)
		task.spawn(load, player)
	end))
	table.insert(connections, Players.PlayerRemoving:Connect(function(player)
		if not isBot(player) then
			save(player)
			profiles[playerId(player)] = nil
		end
	end))

	return {
		profileOf = function(player: Instance): Profile?
			return profiles[playerId(player)]
		end,
		grantKill = function(player: Instance, experience: number, gold: number): number
			local profile = profiles[playerId(player)]
			if profile == nil then
				return 0
			end
			local levels = Progression.grantKill(profile, experience, gold)
			publish(player, profile)
			return levels
		end,
		save = save,
		stop = function()
			stopped = true
			for _, connection in connections do
				connection:Disconnect()
			end
			table.clear(connections)
			for _, player in Players:GetPlayers() do
				if not isBot(player) then
					save(player)
				end
			end
		end,
	}
end

return ProfileStore
