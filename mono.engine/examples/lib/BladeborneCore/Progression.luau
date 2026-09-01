--!strict

local Progression = {}

export type Inventory = { [string]: number }

export type Profile = {
	SchemaVersion: number,
	Level: number,
	Experience: number,
	Gold: number,
	Kills: number,
	HealthFlasks: number,
	EquippedWeapon: string,
	Inventory: Inventory,
}

local CURRENT_SCHEMA = 1
local MAX_LEVEL = 100

local function whole(value: any, fallback: number, minimum: number): number
	if typeof(value) ~= "number" or value ~= value or value == math.huge or value == -math.huge then
		return fallback
	end
	return math.max(minimum, math.floor(value))
end

function Progression.experienceForNextLevel(level: number): number
	local bounded = math.clamp(math.floor(level), 1, MAX_LEVEL)
	return 80 + bounded * bounded * 20
end

function Progression.newProfile(): Profile
	return {
		SchemaVersion = CURRENT_SCHEMA,
		Level = 1,
		Experience = 0,
		Gold = 0,
		Kills = 0,
		HealthFlasks = 3,
		EquippedWeapon = "RustSword",
		Inventory = { RustSword = 1 },
	}
end

function Progression.sanitize(raw: any): Profile
	local profile = Progression.newProfile()
	if typeof(raw) ~= "table" then
		return profile
	end

	profile.Level = math.clamp(whole(raw.Level, 1, 1), 1, MAX_LEVEL)
	profile.Experience = whole(raw.Experience, 0, 0)
	profile.Gold = whole(raw.Gold, 0, 0)
	profile.Kills = whole(raw.Kills, 0, 0)
	profile.HealthFlasks = whole(raw.HealthFlasks, 3, 0)
	if typeof(raw.EquippedWeapon) == "string" and raw.EquippedWeapon ~= "" then
		profile.EquippedWeapon = raw.EquippedWeapon
	end
	if typeof(raw.Inventory) == "table" then
		profile.Inventory = {}
		for itemId, count in raw.Inventory do
			if typeof(itemId) == "string" and itemId ~= "" then
				local amount = whole(count, 0, 0)
				if amount > 0 then
					profile.Inventory[itemId] = amount
				end
			end
		end
	end
	if profile.Inventory[profile.EquippedWeapon] == nil then
		profile.EquippedWeapon = "RustSword"
		profile.Inventory.RustSword = math.max(1, profile.Inventory.RustSword or 0)
	end

	while profile.Level < MAX_LEVEL do
		local required = Progression.experienceForNextLevel(profile.Level)
		if profile.Experience < required then
			break
		end
		profile.Experience -= required
		profile.Level += 1
	end
	if profile.Level == MAX_LEVEL then
		profile.Experience = 0
	end
	return profile
end

function Progression.grantExperience(profile: Profile, amount: number): number
	local gained = whole(amount, 0, 0)
	if gained == 0 or profile.Level >= MAX_LEVEL then
		return 0
	end

	local levels = 0
	profile.Experience += gained
	while profile.Level < MAX_LEVEL do
		local required = Progression.experienceForNextLevel(profile.Level)
		if profile.Experience < required then
			break
		end
		profile.Experience -= required
		profile.Level += 1
		levels += 1
	end
	if profile.Level == MAX_LEVEL then
		profile.Experience = 0
	end
	return levels
end

function Progression.grantKill(profile: Profile, experience: number, gold: number): number
	profile.Kills += 1
	profile.Gold += whole(gold, 0, 0)
	return Progression.grantExperience(profile, experience)
end

function Progression.grantItem(profile: Profile, itemId: string, amount: number): number
	if itemId == "" then
		return 0
	end
	local granted = whole(amount, 0, 0)
	if granted == 0 then
		return profile.Inventory[itemId] or 0
	end
	profile.Inventory[itemId] = (profile.Inventory[itemId] or 0) + granted
	return profile.Inventory[itemId]
end

return Progression
