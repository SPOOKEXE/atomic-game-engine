--!strict

-- The first playable slice of Bladeborne's Sword/Ground light chain.
-- Values are copied from the source game's CombatConfig and CombatMovesets.

local Config = {
	Reach = 9,
	BaseDamage = 10,
	MinimumForwardDot = 0.35,
	InputBuffer = 0.18,
	ComboResetWindow = 1.5,
	Steps = {
		{ Name = "L", Animation = "L1", DamageMultiplier = 1.00, Windup = 0.05, Active = 0.15, Recovery = 0.20 },
		{ Name = "LL", Animation = "L2", DamageMultiplier = 1.05, Windup = 0.05, Active = 0.15, Recovery = 0.18 },
		{ Name = "LLL", Animation = "L3", DamageMultiplier = 1.10, Windup = 0.05, Active = 0.15, Recovery = 0.16 },
		{ Name = "LLLL", Animation = "L_FIN", DamageMultiplier = 1.50, Windup = 0.10, Active = 0.20, Recovery = 0.30 },
	},
}

for _, step in Config.Steps do
	table.freeze(step)
end
table.freeze(Config.Steps)

return table.freeze(Config)
