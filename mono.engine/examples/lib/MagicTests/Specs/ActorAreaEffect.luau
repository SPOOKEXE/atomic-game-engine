--!strict

local function areaConfig(overrides: any?): any
	local config = {
		radius = 10,
		falloff = "Linear",
		relationToSource = "Any",
		includeAnyTags = {},
		includeAllTags = {},
		excludeAnyTags = {},
		maxTargets = 8,
	}
	for key, value in pairs(overrides or {}) do
		config[key] = value
	end
	return config
end

return function(harness: any, core: any, fixtures: any)
	local ActorState = core.ActorState
	local AreaSolver = core.AreaSolver
	local Components = core.Components
	local EffectSolver = core.EffectSolver
	local StatusDefinitions = core.StatusDefinitions

	harness:suite("actor and area solvers", function(h: any)
		h:test("actor builder copies defaults and damage clamps", function()
			local source = { id = "target", position = fixtures.vector(0, 0, 0), health = 30, maxHealth = 100 }
			local actor = ActorState.new(source)
			source.health = 99
			h:assertEqual(actor.health, 30)
			h:assertDeepEqual(actor.velocity, fixtures.vector(0, 0, 0))
			local damaged, delta = ActorState.damage(actor, 50)
			h:assertEqual(damaged.health, 0)
			h:assertEqual(delta.appliedAmount, 30)
			h:assertEqual(actor.health, 30)
		end)

		h:test("healing clamps to max health without mutating input", function()
			local actor = fixtures.actor("target", 0, 70)
			local healed, delta = ActorState.heal(actor, 50)
			h:assertEqual(healed.health, 100)
			h:assertEqual(delta.appliedAmount, 30)
			h:assertEqual(actor.health, 70)
		end)

		h:test("linear area covers center and half but removes boundary and outside", function()
			local result = AreaSolver.solve(areaConfig(), fixtures.vector(0, 0, 0), {
				{ id = "center", position = fixtures.vector(0, 0, 0), tags = {} },
				{ id = "half", position = fixtures.vector(5, 0, 0), tags = {} },
				{ id = "boundary", position = fixtures.vector(10, 0, 0), tags = {} },
				{ id = "outside", position = fixtures.vector(11, 0, 0), tags = {} },
			}, "source")
			h:assertTrue(result.ok)
			h:assertEqual(#result.targets, 2)
			h:assertEqual(result.targets[1].actorId, "center")
			h:assertNear(result.targets[1].falloff, 1)
			h:assertEqual(result.targets[2].actorId, "half")
			h:assertNear(result.targets[2].falloff, 0.5)
			h:assertNear(AreaSolver.falloffAtDistance("Linear", 10, 10), 0)
		end)

		h:test("constant falloff remains one at the boundary", function()
			h:assertNear(AreaSolver.falloffAtDistance("Constant", 10, 10), 1)
			local result = AreaSolver.solve(areaConfig({ falloff = "Constant" }), fixtures.vector(0, 0, 0), {
				{ id = "boundary", position = fixtures.vector(10, 0, 0), tags = {} },
			}, "source")
			h:assertEqual(#result.targets, 1)
			h:assertNear(result.targets[1].falloff, 1)
		end)

		h:test("filters run before target cap", function()
			local result = AreaSolver.solve(areaConfig({
				includeAnyTags = { "wanted" },
				maxTargets = 1,
			}), fixtures.vector(0, 0, 0), {
				{ id = "near_rejected", position = fixtures.vector(1, 0, 0), tags = { "other" } },
				{ id = "far_selected", position = fixtures.vector(5, 0, 0), tags = { "wanted" } },
			}, "source")
			h:assertTrue(result.ok)
			h:assertEqual(#result.targets, 1)
			h:assertEqual(result.targets[1].actorId, "far_selected")
		end)

		h:test("duplicate passing actor IDs fail deterministically", function()
			local result = AreaSolver.solve(areaConfig(), fixtures.vector(0, 0, 0), {
				{ id = "duplicate", position = fixtures.vector(1, 0, 0), tags = {} },
				{ id = "duplicate", position = fixtures.vector(2, 0, 0), tags = {} },
			}, "source")
			h:assertEqual(result.ok, false)
			h:assertEqual(result.error, "DuplicateActorId")
			h:assertEqual(result.actorId, "duplicate")
		end)

		h:test("distance and actor ID make insertion order irrelevant", function()
			local first = {
				{ id = "beta", position = fixtures.vector(2, 0, 0), tags = {} },
				{ id = "alpha", position = fixtures.vector(-2, 0, 0), tags = {} },
			}
			local second = { first[2], first[1] }
			local firstResult = AreaSolver.solve(areaConfig(), fixtures.vector(0, 0, 0), first, "source")
			local secondResult = AreaSolver.solve(areaConfig(), fixtures.vector(0, 0, 0), second, "source")
			h:assertDeepEqual(firstResult, secondResult)
			h:assertEqual(firstResult.targets[1].actorId, "alpha")
		end)

		h:test("faction authority ignores caller-provided relation hints", function()
			local candidates = {
				{
					id = "ally",
					position = fixtures.vector(2, 0, 0),
					tags = {},
					factionId = "blue",
					relationToSource = "Enemy",
				},
				{
					id = "enemy",
					position = fixtures.vector(3, 0, 0),
					tags = {},
					factionId = "red",
					relationToSource = "Ally",
				},
			}
			local allies = AreaSolver.solve(
				areaConfig({ relationToSource = "Ally", falloff = "Constant" }),
				fixtures.vector(0, 0, 0),
				candidates,
				"source",
				"blue"
			)
			h:assertEqual(#allies.targets, 1)
			h:assertEqual(allies.targets[1].actorId, "ally")
			h:assertEqual(allies.targets[1].relationToSource, "Ally")

			local enemies = AreaSolver.solve(
				areaConfig({ relationToSource = "Enemy", falloff = "Constant" }),
				fixtures.vector(0, 0, 0),
				candidates,
				"source",
				"blue"
			)
			h:assertEqual(#enemies.targets, 1)
			h:assertEqual(enemies.targets[1].actorId, "enemy")
			h:assertEqual(enemies.targets[1].relationToSource, "Enemy")
		end)
	end)

	harness:suite("effect solver", function(h: any)
		local function newSolver(definitions: { any }?, maxQueuedStatusOperations: number?): any
			return EffectSolver.new({
				actors = {
					fixtures.actor("source", 0, 100, { "caster" }, "blue"),
					fixtures.actor("target", 10, 50, { "enemy" }, "red"),
					fixtures.actor("ally", 5, 60, { "ally" }, "blue"),
				},
				statusDefinitions = definitions or {},
				maxQueuedStatusOperations = maxQueuedStatusOperations,
			})
		end

		h:test("damage and heal use one authoritative reducer", function()
			local effectSolver = newSolver()
			local damage = effectSolver:execute(Components.Effect.Damage(30, "Arcane", { "spell" }), fixtures.context("damage", "target"))
			local damageEvent = fixtures.findEvent(damage.events, "DamageApplied")
			h:assertEqual(damageEvent.beforeHealth, 50)
			h:assertEqual(damageEvent.afterHealth, 20)
			h:assertEqual(damageEvent.castId, "cast.test")
			local heal = effectSolver:execute(Components.Effect.Heal(200), fixtures.context("heal", "target"))
			local healEvent = fixtures.findEvent(heal.events, "HealApplied")
			h:assertEqual(healEvent.afterHealth, 100)
			h:assertEqual(healEvent.appliedAmount, 80)
		end)

		h:test("area applies falloff through the child public path", function()
			local effectSolver = newSolver()
			local area = Components.Effect.Area("Impact", 20, { Components.Effect.Damage(40) }, {
				falloff = "Linear",
				relationToSource = "Enemy",
				maxTargets = 2,
			})
			local context = fixtures.context("area", nil)
			context.areaCandidates = {
				{ id = "target", relationToSource = "Ally" },
				{ id = "ally", relationToSource = "Enemy" },
			}
			local result = effectSolver:execute(area, context)
			local selected = fixtures.findEvent(result.events, "AreaSelected")
			local damage = fixtures.findEvent(result.events, "DamageApplied")
			h:assertEqual(#selected.targets, 1)
			h:assertEqual(selected.targets[1].actorId, "target")
			h:assertNear(damage.requestedAmount, 20)
			h:assertEqual(effectSolver:getActor("target").health, 30)
		end)

		h:test("pull emits an inward bounded command", function()
			local effectSolver = newSolver()
			local result = effectSolver:execute(Components.Effect.Pull("Impact", 8, 5), fixtures.context("pull", "target"))
			h:assertEqual(#result.commands, 1)
			h:assertEqual(result.commands[1].kind, "PullCommand")
			h:assertNear(result.commands[1].deltaSpeed, 5)
			h:assertNear(result.commands[1].deltaVelocity.x, -5)
			h:assertNear(result.commands[1].deltaVelocity.y, 0)
		end)

		h:test("pull at center returns a structured skip", function()
			local effectSolver = newSolver()
			local context = fixtures.context("pull_zero", "source")
			local result = effectSolver:execute(Components.Effect.Pull("Impact", 8, 8), context)
			local skipped = fixtures.findEvent(result.events, "PullSkipped")
			h:assertEqual(skipped.reason, "ZeroDistance")
			h:assertEqual(#result.commands, 0)
		end)

		h:test("apply status and cleanse queue until an explicit controlled advance", function()
			local effectSolver = newSolver({ StatusDefinitions.burn() })
			local queuedApply = effectSolver:execute(
				Components.Effect.ApplyStatus("burn", 1, 1, 1),
				fixtures.context("apply_burn", "target")
			)
			h:assertTrue(fixtures.findEvent(queuedApply.events, "StatusCommandQueued") ~= nil)
			h:assertNil(fixtures.findEvent(queuedApply.events, "StatusApplied"))
			h:assertEqual(#effectSolver:getStatuses("target"), 0)
			local applied = effectSolver:advance(0, 0.25)
			h:assertTrue(fixtures.findEvent(applied.events, "StatusApplied") ~= nil)
			h:assertEqual(#effectSolver:getStatuses("target"), 1)
			local queuedCleanse = effectSolver:execute(Components.Effect.Cleanse({
				includeAnyTags = { "burn" },
				maxRemovals = 1,
			}), fixtures.context("cleanse_burn", "target", 1))
			h:assertTrue(fixtures.findEvent(queuedCleanse.events, "StatusCommandQueued") ~= nil)
			h:assertEqual(#effectSolver:getStatuses("target"), 1)
			local cleanse = effectSolver:advance(1, 1.25)
			h:assertTrue(fixtures.findEvent(cleanse.events, "StatusCleansed") ~= nil)
			h:assertEqual(#effectSolver:getStatuses("target"), 0)
		end)

		h:test("batch preflight blocks direct damage when status queue cannot reserve lifecycle", function()
			local effectSolver = newSolver({ StatusDefinitions.burn() }, 1)
			local requests = {
				{
					effect = Components.Effect.Damage(10),
					context = fixtures.context("atomic_damage", "target"),
				},
				{
					effect = Components.Effect.ApplyStatus("burn", 1, 1, 1),
					context = fixtures.context("atomic_status", "target"),
				},
			}
			local blocked = effectSolver:executeBatch(requests)
			h:assertEqual(blocked.ok, false)
			h:assertEqual(blocked.failureReason, "QueueFull")
			h:assertEqual(blocked.processedEffects, 0)
			h:assertEqual(effectSolver:getActor("target").health, 50)
			h:assertNil(effectSolver._operationLedger.atomic_damage)
			h:assertNil(effectSolver._operationLedger.atomic_status)

			effectSolver._statusSolver._maxQueuedOperations = 3
			local retry = effectSolver:executeBatch(requests)
			h:assertTrue(retry.ok)
			h:assertEqual(effectSolver:getActor("target").health, 40)
			h:assertTrue(fixtures.findEvent(retry.events, "StatusCommandQueued") ~= nil)
		end)

		h:test("separate same-time cleanse and apply obey phase order after one advance", function()
			local effectSolver = newSolver({ StatusDefinitions.burn() })
			effectSolver:execute(
				Components.Effect.ApplyStatus("burn", 1, 1, 1),
				fixtures.context("phase_old", "target", 0)
			)
			effectSolver:advance(0)

			effectSolver:execute(
				Components.Effect.ApplyStatus("burn", 1, 1, 1),
				fixtures.context("phase_new", "target", 1)
			)
			effectSolver:execute(Components.Effect.Cleanse({
				includeAnyTags = { "burn" },
				maxRemovals = 1,
			}), fixtures.context("phase_cleanse", "target", 1))

			local result = effectSolver:advance(1)
			local cleansedIndex: number? = nil
			local appliedIndex: number? = nil
			for index, event in ipairs(result.events) do
				if event.kind == "StatusCleansed" then
					cleansedIndex = index
				elseif event.kind == "StatusApplied" then
					appliedIndex = index
				end
			end
			h:assertTrue(cleansedIndex ~= nil and appliedIndex ~= nil and cleansedIndex < appliedIndex)
			h:assertEqual(#effectSolver:getStatuses("target"), 1)
		end)

		h:test("duplicate effect operation is ignored", function()
			local effectSolver = newSolver()
			local effect = Components.Effect.Damage(10)
			effectSolver:execute(effect, fixtures.context("same", "target"))
			local duplicate = effectSolver:execute(effect, fixtures.context("same", "target"))
			h:assertEqual(duplicate.ok, false)
			h:assertEqual(duplicate.failureReason, "DuplicateOperationId")
			h:assertTrue(fixtures.findEvent(duplicate.events, "EffectDuplicateIgnored") ~= nil)
			h:assertEqual(effectSolver:getActor("target").health, 40)
		end)
	end)
end
