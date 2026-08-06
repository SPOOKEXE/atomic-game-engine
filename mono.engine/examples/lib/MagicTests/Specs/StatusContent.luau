--!strict

-- The two shipped status definitions that reach paths nothing shipped reached
-- before: `chill` carries modifiers and replaces if stronger, `laceration`
-- stacks independently.
--
-- `StatusEffects.luau` already covers the solver's *policies* using ad-hoc
-- fixtures. What it cannot cover is whether any content ever takes them, and the
-- answer until now was no: burn and poison are `AddStacks` and `RefreshDuration`
-- with `modifiers = {}` between them, so a regression that dropped modifiers on
-- the floor would have passed every test in this repository.

return function(harness: any, core: any, fixtures: any)
	local ActorState = core.ActorState
	local Enums = core.Enums
	local StatusDefinitions = core.StatusDefinitions
	local StatusEffectSolver = core.StatusEffectSolver
	local Presets = core.Presets

	local function solverWith(definitions: { any }): (any, { [string]: any })
		local actors = {
			source = ActorState.new(fixtures.actor("source", 0, 100)),
			target = ActorState.new(fixtures.actor("target", 0, 100)),
		}
		return StatusEffectSolver.new(definitions, actors), actors
	end

	-- Queue one application and step the solver to it, which is the shape every
	-- other status spec uses.
	local function apply(solver: any, definition: any, operationId: string, potency: number, at: number): any
		solver:queueApply({
			operationId = operationId,
			castId = "cast.status",
			sourceActorId = "source",
			targetActorId = "target",
			scheduledTime = at,
			definitionId = definition.id,
			definitionVersion = definition.version,
			stacks = 1,
			potency = potency,
		})
		return solver:advance(at)
	end

	harness:suite("chill definition", function(h: any)
		h:test("carries the modifiers a shipped definition never had", function()
			local chill = StatusDefinitions.chill()
			h:assertEqual(#chill.modifiers, 2)
			h:assertEqual(chill.modifiers[1].stat, Enums.Stat.MoveSpeed)
			h:assertEqual(chill.modifiers[1].operation, "Multiply")
			h:assertTrue(chill.modifiers[1].value < 1, "a slow must reduce speed")
		end)

		h:test("the slow is multiplicative, not additive", function()
			-- An additive slow larger than a slow actor's base speed walks them
			-- backwards. Asserted because the two operations are one word apart.
			for _, modifier in StatusDefinitions.chill().modifiers do
				if modifier.stat == Enums.Stat.MoveSpeed then
					h:assertEqual(modifier.operation, "Multiply")
				end
			end
		end)

		h:test("replaces if stronger rather than stacking or refreshing", function()
			h:assertEqual(StatusDefinitions.chill().stackPolicy, "ReplaceIfStronger")
			h:assertEqual(StatusDefinitions.chill().maxIndependentInstances, 1)
		end)

		h:test("burn and poison are unchanged by any of this", function()
			-- The definitions the rest of the content depends on. Adding two more
			-- to the table is exactly the kind of edit that quietly alters one.
			h:assertEqual(StatusDefinitions.burn().stackPolicy, "AddStacks")
			h:assertEqual(StatusDefinitions.poison().stackPolicy, "RefreshDuration")
			h:assertEqual(#StatusDefinitions.burn().modifiers, 0)
			h:assertEqual(#StatusDefinitions.poison().modifiers, 0)
		end)

		h:test("all() lists every definition exactly once", function()
			local seen: { [string]: number } = {}
			for _, definition in StatusDefinitions.all() do
				seen[definition.id] = (seen[definition.id] or 0) + 1
			end
			h:assertEqual(seen[Enums.StatusId.Burn], 1)
			h:assertEqual(seen[Enums.StatusId.Poison], 1)
			h:assertEqual(seen[Enums.StatusId.Chill], 1)
			h:assertEqual(seen[Enums.StatusId.Laceration], 1)
		end)
	end)

	harness:suite("laceration definition", function(h: any)
		h:test("stacks independently with a reachable cap", function()
			local laceration = StatusDefinitions.laceration()
			h:assertEqual(laceration.stackPolicy, "Independent")
			h:assertTrue(laceration.maxIndependentInstances > 1)
			-- Splinterstorm throws eight shards at a six-instance cap, which is
			-- what makes IndependentCap something shipped content produces.
			h:assertTrue(Presets.get("Splinterstorm").burst.children[1].count > laceration.maxIndependentInstances)
		end)

		h:test("the cap is enforced and the overflow is named", function()
			local laceration = StatusDefinitions.laceration()
			local solver = solverWith({ laceration })

			for index = 1, laceration.maxIndependentInstances do
				local result = apply(solver, laceration, "op." .. index, 1, index * 0.01)
				h:assertTrue(fixtures.findEvent(result.events, "StatusApplied") ~= nil, "instance " .. index .. " should have applied")
			end

			local overflow = apply(solver, laceration, "op.overflow", 1, 0.5)
			local rejected = fixtures.findEvent(overflow.events, "StatusRejected")
			h:assertTrue(rejected ~= nil, "the cap should reject rather than silently drop")
			h:assertEqual(rejected.reason, "IndependentCap")
		end)

		h:test("each instance carries the definition's modifiers", function()
			-- The path that had never carried a value: the solver deep-copies
			-- definition.modifiers into every StatusModifiersChanged command.
			local laceration = StatusDefinitions.laceration()
			local solver = solverWith({ laceration })
			local result = apply(solver, laceration, "op.one", 1, 0)

			local command = fixtures.findEvent(result.commands, "StatusModifiersChanged")
			h:assertTrue(command ~= nil, "applying a status should emit a modifier command")
			h:assertEqual(command.action, "Add")
			h:assertEqual(#command.modifiers, #laceration.modifiers)
			h:assertEqual(command.modifiers[1].stat, Enums.Stat.MoveSpeed)
		end)
	end)

	harness:suite("chill replacement ordering", function(h: any)
		h:test("a stronger application wins and a weaker one is discarded", function()
			local chill = StatusDefinitions.chill()
			local solver = solverWith({ chill })

			apply(solver, chill, "op.weak", 1, 0)
			local stronger = apply(solver, chill, "op.strong", 5, 1)
			h:assertTrue(fixtures.findEvent(stronger.events, "StatusReplaced") ~= nil,
				"a stronger chill should replace the weaker instance")

			-- The weaker one that follows must not overwrite the stronger, and it
			-- is a `StatusMergeNoOp` rather than a `StatusRejected`. The two are
			-- worth keeping apart: a rejection means a cap was hit and something
			-- was lost, a no-op means the policy looked at both and kept the one
			-- it had. A caller retrying on rejection would loop forever on this.
			local weaker = apply(solver, chill, "op.weaker", 2, 2)
			local noop = fixtures.findEvent(weaker.events, "StatusMergeNoOp")
			h:assertTrue(noop ~= nil, "a weaker chill should not replace a stronger one")
			h:assertEqual(noop.reason, "NotStronger")
			h:assertNil(fixtures.findEvent(weaker.events, "StatusRejected"))
		end)

		h:test("the lance applies chill at a potency other than one", function()
			-- Potency 2 is what makes the replacement ordering observable at all:
			-- at the default of 1 every application ties, and a policy regression
			-- to RefreshDuration would be invisible.
			local document = Presets.get("GlacialLance")
			local found = false
			for _, trigger in document.effectTriggers do
				for _, effect in trigger.effect.effects or {} do
					if effect.kind == Enums.EffectKind.ApplyStatus then
						h:assertEqual(effect.definitionId, Enums.StatusId.Chill)
						h:assertTrue(effect.potency > 1, "chill should be applied above the default potency")
						found = true
					end
				end
			end
			h:assertTrue(found, "the lance should apply a status")
		end)
	end)
end
