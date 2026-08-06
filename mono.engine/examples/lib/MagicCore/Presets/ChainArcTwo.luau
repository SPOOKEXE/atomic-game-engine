--!strict

local ChainArcLink = require(script.Parent.ChainArcLink)

-- The second link of `ChainLightning`. See `ChainArcLink` for the shape.
return function(): { [string]: any }
	return ChainArcLink({
		id = "chain_arc_two",
		nextRecipeId = "chain_arc_three",
		lifetime = 0.34,
		speed = 190,
		damage = 34,
		radius = 10,
		thickness = 0.3,
	})
end
