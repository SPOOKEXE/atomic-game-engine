--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)

return function(): { [string]: any }
	local document = Builder.new("meteor_child_cinder", 1)
	document = Builder.withProjectile(document, Builder.projectile(
		Components.Lifetime.Time(1.1),
		Components.Motion.Linear({ x = 0, y = 6, z = -48 }),
		Components.Presentation.Part({
			size = { x = 0.55, y = 0.55, z = 0.55 },
			color = { r = 1, g = 0.55, b = 0.08 },
			material = "Neon",
			particles = { Components.Presentation.Particle("cinder_tail", 0.8, 0.08, 0.25, { rate = 8 }) },
		})
	))
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("cinder_impact", Enums.Trigger.Impact, 1,
			Components.Effect.Damage(14, Enums.DamageType.Fire, { Enums.EffectTag.Fire, Enums.EffectTag.Meteor }))
	})
	return document
end
