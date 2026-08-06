--!strict

local ChainArcLink = require(script.Parent.ChainArcLink)

-- The third link of `ChainLightning`. See `ChainArcLink` for the shape.
return function(): { [string]: any }
	return ChainArcLink({
		id = "chain_arc_three",
		nextRecipeId = "chain_arc_four",
		lifetime = 0.28,
		speed = 165,
		damage = 24,
		radius = 9,
		thickness = 0.26,
	})
end
