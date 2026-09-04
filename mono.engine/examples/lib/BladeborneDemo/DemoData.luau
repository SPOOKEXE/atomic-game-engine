--!strict
-- Local fixture replacing Bladeborne's replicated player, quest and combat data.

local DemoData = {
	profile = {
		name = "Spook", initial = "S", level = 2, floor = 0, zone = "FLOOR 0",
		health = 150, maxHealth = 150, mana = 0, maxMana = 100,
		experience = 981, maxExperience = 1000, currency = 450, time = "11:38",
		quest = {
			name = "Menu Test Quest A",
			body = "Placeholder quest used to populate the quest menu during UI development.",
			objective = "Second placeholder step", distance = "2 / 4",
		},
		abilities = {
			{ key = "Q", glyph = "X", name = "Dual Blades", cooldown = "", state = "READY", active = true },
			{ key = "W", glyph = "*", name = "Star Skill", cooldown = "", state = "READY", active = false },
			{ key = "R", glyph = "O", name = "Solar Guard", cooldown = "", state = "READY", active = false },
			{ key = "D", glyph = "+", name = "Recovery", cooldown = "", state = "READY", active = false },
			{ key = "F", glyph = "A", name = "Ascension", cooldown = "", state = "READY", active = false },
			{ key = "G", glyph = "C", name = "Cinematic Test", cooldown = "", state = "READY", active = false },
		},
	},
	hotbar = {
		primary = {
			{ key = "1", glyph = "S", active = true }, { key = "2", glyph = "B" }, { key = "3", glyph = "F" },
			{ key = "4", glyph = "W" }, { key = "5", glyph = "A" }, { key = "6", glyph = "L" },
			{ key = "7", glyph = "C" }, { key = "8", glyph = "*" }, { key = "9", glyph = "!" },
			{ key = "0", glyph = "H" }, { key = "-", glyph = "" }, { key = "=", glyph = "" },
		},
		secondary = {
			{ key = "U", glyph = "" }, { key = "I", glyph = "" }, { key = "O", glyph = "" },
			{ key = "P", glyph = "" }, { key = "H", glyph = "" }, { key = "J", glyph = "" },
			{ key = "K", glyph = "" }, { key = "L", glyph = "" }, { key = "V", glyph = "" },
			{ key = "B", glyph = "" }, { key = "N", glyph = "" }, { key = "M", glyph = "" },
		},
	},
	effects = {
		{ glyph = "B", time = "0:48", colour = Color3.fromRGB(243, 192, 89) },
		{ glyph = "W", time = "2:13", colour = Color3.fromRGB(66, 218, 206) },
		{ glyph = ">", time = "0:12", colour = Color3.fromRGB(100, 224, 151) },
	},
}

return table.freeze(DemoData)
