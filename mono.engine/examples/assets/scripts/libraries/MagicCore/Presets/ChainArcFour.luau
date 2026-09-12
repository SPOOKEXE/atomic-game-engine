--!strict

local ChainArcLink = require(script.Parent.ChainArcLink)

-- The last link of `ChainLightning`: no `nextRecipeId`, so no burst at all.
return function(): { [string]: any }
	return ChainArcLink({
		id = "chain_arc_four",
		nextRecipeId = nil,
		lifetime = 0.22,
		speed = 140,
		damage = 17,
		radius = 8,
		thickness = 0.22,
	})
end
