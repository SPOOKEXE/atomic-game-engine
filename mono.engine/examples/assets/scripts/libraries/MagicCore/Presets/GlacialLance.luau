--!strict

local MagicCore = script.Parent.Parent
local Builder = require(MagicCore.Builder)
local Components = require(MagicCore.Components)
local Enums = require(MagicCore.Enums)
local StatusDefinitions = require(MagicCore.StatusDefinitions)

-- A shard of ice that slows what it hits and keeps slowing it.
--
-- **This preset leans on the status solver rather than the projectile solver**,
-- which is the half of the engine the shipped content had barely touched. Every
-- other preset either does damage on impact or throws children; the interesting
-- work here happens after the projectile is already gone.
--
-- Two paths it is the first shipped content to take, both through `chill`:
--
--   * **status modifiers.** `StatusEffectSolver` copies `definition.modifiers`
--     into every `StatusModifiersChanged` command it emits, and burn and poison
--     both declare an empty list -- so the field round-tripped through the
--     compiler, the validator and the solver without ever carrying a value.
--     `chill` carries two.
--   * **`ReplaceIfStronger`.** Burn adds stacks, poison refreshes; neither
--     reaches the potency compare. Two lances landing on one target should leave
--     the stronger slow rather than the sum or the newer, which is exactly what
--     that policy is.
--
-- ## Why the lance expires rather than terminating on impact
--
-- The shatter is an `Expire` trigger as well as an `Impact` one, and both apply
-- the same status. A lance that only chilled on a hit did nothing at all over
-- open ground, which for a spell whose point is the after-effect reads as the
-- status system being broken rather than as a miss.
return function(): { [string]: any }
	local projectile = Builder.projectile(
		Components.Lifetime.Time(1.4),
		-- A shallow arc: heavy enough to read as a thrown spike, flat enough
		-- that it still crosses a lane before expiring.
		Components.Motion.Ballistic(
			{ x = 0, y = 9, z = -96 },
			{ x = 0, y = -22, z = 0 }
		),
		Components.Presentation.Part({
			shape = "Block",
			size = { x = 0.5, y = 0.5, z = 2.6 },
			color = { r = 0.66, g = 0.88, b = 0.98 },
			material = "Glass",
			transparency = 0.25,
			particles = {
				Components.Presentation.Particle("frost_tail", 1.4, 0.25, 0.7, {
					rate = 40,
				}),
				Components.Presentation.Particle("frost_mist", 1.4, 0.6, 1.5, {
					rate = 18,
				}),
				-- Cue emitters: rate zero and no timeline entry, so the runtime
				-- fires them at whatever point the lance actually stopped.
				Components.Presentation.Particle("frost_impact_shards", 0, 0.3, 0.9, {
					rate = 0,
				}),
				Components.Presentation.Particle("frost_impact_ring", 0, 0.4, 1.2, {
					rate = 0,
				}),
			},
			curves = {
				Components.Presentation.Curve("frost_grow", "Scale", "NormalizedLifetime", {
					{ time = 0, value = 0.55 },
					{ time = 0.3, value = 1 },
					{ time = 1, value = 1.1 },
				}),
				Components.Presentation.Curve("frost_fade", "Transparency", "NormalizedLifetime", {
					{ time = 0, value = 0.25 },
					{ time = 0.85, value = 0.3 },
					{ time = 1, value = 1 },
				}),
			},
			timeline = {
				Components.Timeline.SetEmitterEnabled("frost_mist_off", 1.2, "frost_mist", false),
			},
		})
	)

	-- The chill is applied through an Area rather than to the struck actor
	-- alone, because a shard of ice bursts. `maxTargets` is small: this is a
	-- spray of splinters, not a nova.
	local shatter = Components.Effect.Area(Enums.AreaCenter.Impact, 16, {
		Components.Effect.Damage(46, Enums.DamageType.Ice, {
			Enums.EffectTag.Ice,
			Enums.EffectTag.Area,
			Enums.EffectTag.Spell,
		}),
		-- Potency 2 rather than 1, so a second lance landing at potency 1 is
		-- discarded by `ReplaceIfStronger` and one at potency 3 wins. That
		-- ordering is the whole behaviour and it is invisible unless the number
		-- is deliberate.
		Components.Effect.ApplyStatus(Enums.StatusId.Chill, 1, 1, 2),
	}, {
		falloff = Enums.AreaFalloff.Linear,
		relationToSource = Enums.Relation.Enemy,
		maxTargets = 6,
	})

	local document = Builder.new("glacial_lance", 1)
	document = Builder.withProjectile(document, projectile)
	document = Builder.withEvents(document, {
		Components.Event.Spawn("spawn", 1, Enums.EventAction.None),
		Components.Event.Impact("impact", 2, Enums.EventAction.Terminate),
		Components.Event.Expire("expire", 3, Enums.EventAction.Terminate),
	})
	document = Builder.withStatusDefinitions(document, {
		StatusDefinitions.chill(),
	})
	document = Builder.withEffectTriggers(document, {
		Components.Effect.Trigger("shatter", Enums.Trigger.Impact, 1, shatter, { "spell.glacial_lance.shatter" }),
		Components.Effect.Trigger("airburst", Enums.Trigger.Expire, 2, shatter, { "spell.glacial_lance.airburst" }),
	})
	document = Builder.withBudgets(document, {
		maxProjectiles = 1,
		maxEvents = 20,
		maxParticles = 192,
		maxBurstDepth = 1,
		maxEffectCommands = 96,
		maxAreaTargets = 12,
		-- **Twelve, and getting here took two wrong answers worth recording.**
		--
		-- "One" looks right: `ReplaceIfStronger` means a target never holds two
		-- chills at once. "Two" looks right next: the lance applies the status
		-- from an Impact trigger and an Expire trigger. Both are refused,
		-- because despite the diagnostic saying "to one target" the cost is
		-- computed as `targetCount * perTargetInstances` summed over the
		-- triggers -- an Area of six times one status, twice, is twelve.
		--
		-- So this budget bounds the *fan-out of the data*, not what any actor
		-- ends up carrying. Reasoning about the stack policy to pick it is
		-- reasoning about the wrong thing, and the validator is right to refuse
		-- a number arrived at that way.
		maxStatusesPerActor = 12,
		maxStatusTicksPerAdvance = 16,
	})
	return document
end
