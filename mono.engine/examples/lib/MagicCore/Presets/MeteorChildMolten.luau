--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

return function(): { [string]: any }
	local document = Builder.new("meteor_child_molten", 1)
	document = Builder.withProjectile(document, Builder.projectile(
		Components.Lifetime.Time(1.4),
		Components.Motion.Ballistic({ x = 0, y = 18, z = -42 }, { x = 0, y = -36, z = 0 }),
		Components.Presentation.Part({
			size = { x = 0.9, y = 0.9, z = 0.9 },
			color = { r = 1, g = 0.2, b = 0.03 },
			material = "Neon",
			particles = { Components.Presentation.Particle("molten_tail", 1.1, 0.1, 0.35, { rate = 12 }) },
		})
	))
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("molten_impact", Enums.Trigger.Impact, 1,
			Components.Effect.Damage(24, Enums.DamageType.Fire, { Enums.EffectTag.Fire, Enums.EffectTag.Meteor }))
	})
	return document
end
