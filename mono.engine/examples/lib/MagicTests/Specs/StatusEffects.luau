--!strict

return function(harness: any, core: any, fixtures: any)
	local ActorState = core.ActorState
	local Components = core.Components
	local StatusDefinitions = core.StatusDefinitions
	local StatusEffectSolver = core.StatusEffectSolver

	local function makeSolver(definitions: { any }, config: any?): (any, { [string]: any })
		local actors = {
			source = ActorState.new(fixtures.actor("source", 0, 100)),
			target = ActorState.new(fixtures.actor("target", 0, 100)),
		}
		return StatusEffectSolver.new(definitions, actors, config), actors
	end

	local function applyInput(operationId: string, definitionId: string, scheduledTime: number, extra: any?): any
		local input = {
			operationId = operationId,
			castId = "cast.status",
			sourceActorId = "source",
			targetActorId = "target",
			scheduledTime = scheduledTime,
			definitionId = definitionId,
			definitionVersion = 1,
			stacks = 1,
			potency = 1,
		}
		for key, value in pairs(extra or {}) do
			input[key] = value
		end
		return input
	end

	harness:suite("status effect solver", function(h: any)
		h:test("poison ticks on apply with pinned attribution", function()
			local statusSolver, actors = makeSolver({ StatusDefinitions.poison() })
			local ok = statusSolver:queueApply(applyInput("poison_apply", "poison", 0))
			h:assertTrue(ok)
			local result = statusSolver:advance(0)
			local tick = fixtures.findEvent(result.events, "StatusTick")
			local damage = fixtures.findEvent(result.events, "DamageApplied")
			h:assertEqual(tick.tickOrdinal, 0)
			h:assertEqual(damage.sourceActorId, "source")
			h:assertEqual(damage.targetActorId, "target")
			h:assertEqual(damage.castId, "cast.status")
			h:assertEqual(actors.target.health, 97)
		end)

		h:test("burn AddStacks merges before its same-time tick", function()
			local statusSolver, actors = makeSolver({ StatusDefinitions.burn() })
			statusSolver:queueApply(applyInput("burn_one", "burn", 0))
			statusSolver:advance(0)
			statusSolver:queueApply(applyInput("burn_two", "burn", 1))
			local result = statusSolver:advance(1)
			local merged = fixtures.findEvent(result.events, "StatusMerged")
			local tick = fixtures.findEvent(result.events, "StatusTick")
			h:assertEqual(merged.instance.stacks, 2)
			h:assertEqual(tick.tickOrdinal, 1)
			h:assertEqual(actors.target.health, 92)
		end)

		h:test("RefreshDuration keeps stacks and takes maximum potency", function()
			local statusSolver = makeSolver({ StatusDefinitions.poison() })
			statusSolver:queueApply(applyInput("refresh_one", "poison", 0, { potency = 2 }))
			statusSolver:advance(0)
			statusSolver:queueApply(applyInput("refresh_two", "poison", 1, { stacks = 1, potency = 1 }))
			local result = statusSolver:advance(1)
			local merged = fixtures.findEvent(result.events, "StatusMerged")
			h:assertEqual(merged.stackPolicy, "RefreshDuration")
			h:assertEqual(merged.instance.stacks, 1)
			h:assertEqual(merged.instance.potency, 2)
			h:assertNear(merged.instance.expiresAt, 9)
		end)

		h:test("AddStacks clamps to definition maximum", function()
			local definition = fixtures.statusDefinition(Components, "stacking", "AddStacks", { maxStacks = 3 })
			local statusSolver = makeSolver({ definition })
			statusSolver:queueApply(applyInput("stack_one", "stacking", 0, { stacks = 2 }))
			statusSolver:advance(0)
			statusSolver:queueApply(applyInput("stack_two", "stacking", 1, { stacks = 2 }))
			statusSolver:advance(1)
			h:assertEqual(statusSolver:getStatuses("target")[1].stacks, 3)
		end)

		h:test("ReplaceIfStronger uses potency then later expiry", function()
			local definition = fixtures.statusDefinition(Components, "replace", "ReplaceIfStronger", { duration = 4 })
			local statusSolver = makeSolver({ definition })
			statusSolver:queueApply(applyInput("replace_one", "replace", 0, { potency = 2 }))
			statusSolver:advance(0)
			statusSolver:queueApply(applyInput("replace_weak", "replace", 0.5, { potency = 2, durationOverride = 1 }))
			local weak = statusSolver:advance(0.5)
			h:assertTrue(fixtures.findEvent(weak.events, "StatusMergeNoOp") ~= nil)
			statusSolver:queueApply(applyInput("replace_later", "replace", 0.6, { potency = 2, durationOverride = 5 }))
			local later = statusSolver:advance(0.6)
			h:assertTrue(fixtures.findEvent(later.events, "StatusReplaced") ~= nil)
			h:assertNear(statusSolver:getStatuses("target")[1].expiresAt, 5.6)
		end)

		h:test("Independent creates instances up to its exact cap", function()
			local definition = fixtures.statusDefinition(Components, "independent", "Independent", {
				maxIndependentInstances = 2,
			})
			local statusSolver = makeSolver({ definition })
			statusSolver:queueApply(applyInput("independent_one", "independent", 0))
			statusSolver:queueApply(applyInput("independent_two", "independent", 0))
			statusSolver:queueApply(applyInput("independent_three", "independent", 0))
			local result = statusSolver:advance(0)
			h:assertEqual(#statusSolver:getStatuses("target"), 2)
			local rejected = fixtures.findEvent(result.events, "StatusRejected")
			h:assertEqual(rejected.reason, "IndependentCap")
		end)

		h:test("same-time cleanse runs before apply", function()
			local statusSolver = makeSolver({ StatusDefinitions.burn() })
			statusSolver:queueApply(applyInput("phase_old", "burn", 0))
			statusSolver:advance(0)
			statusSolver:queueApply(applyInput("phase_new", "burn", 1))
			statusSolver:queueCleanse({
				operationId = "phase_cleanse",
				castId = "cast.status",
				sourceActorId = "source",
				targetActorId = "target",
				scheduledTime = 1,
				includeAnyTags = { "burn" },
				includeAllTags = {},
				excludeAnyTags = {},
				maxRemovals = 1,
			})
			local result = statusSolver:advance(1)
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
			h:assertEqual(#statusSolver:getStatuses("target"), 1)
		end)

		h:test("refresh at expiry invalidates old expiry before tick", function()
			local definition = fixtures.statusDefinition(Components, "expiry_refresh", "RefreshDuration", {
				duration = 1,
				tickInterval = 1,
				tickAtExpiry = true,
				periodicEffects = { Components.Effect.Damage(1) },
			})
			local statusSolver = makeSolver({ definition })
			statusSolver:queueApply(applyInput("expiry_first", "expiry_refresh", 0))
			statusSolver:advance(0)
			statusSolver:queueApply(applyInput("expiry_refresh", "expiry_refresh", 1))
			local result = statusSolver:advance(1, 1.25)
			local merged = fixtures.findEvent(result.events, "StatusMerged")
			h:assertTrue(merged.refreshedAtExpiry)
			h:assertTrue(fixtures.findEvent(result.events, "StatusTick") ~= nil)
			h:assertNil(fixtures.findEvent(result.events, "StatusExpired"))
			h:assertNear(statusSolver:getStatuses("target")[1].expiresAt, 2)
		end)

		h:test("tick ordinals include apply and aligned expiry without drift", function()
			local definition = fixtures.statusDefinition(Components, "ordinals", "RefreshDuration", {
				duration = 2,
				tickInterval = 1,
				tickOnApply = true,
				tickAtExpiry = true,
				periodicEffects = { Components.Effect.Damage(1) },
			})
			local statusSolver = makeSolver({ definition })
			statusSolver:queueApply(applyInput("ordinal_apply", "ordinals", 0))
			local result = statusSolver:advance(2, 2.5)
			local ticks = fixtures.eventsOfKind(result.events, "StatusTick")
			h:assertEqual(#ticks, 3)
			for ordinal = 0, 2 do
				h:assertEqual(ticks[ordinal + 1].tickOrdinal, ordinal)
				h:assertNear(ticks[ordinal + 1].scheduledTime, ordinal)
				h:assertNear(ticks[ordinal + 1].processedAt, 2.5)
			end
			h:assertTrue(ticks[3].atExpiry)
			h:assertTrue(fixtures.findEvent(result.events, "StatusExpired") ~= nil)
		end)

		h:test("duplicate operation IDs are rejected before enqueue", function()
			local statusSolver = makeSolver({ StatusDefinitions.poison() })
			local input = applyInput("duplicate", "poison", 0)
			local firstOk = statusSolver:queueApply(input)
			local secondOk, reason = statusSolver:queueApply(input)
			h:assertTrue(firstOk)
			h:assertEqual(secondOk, false)
			h:assertEqual(reason, "DuplicateOperationId")
		end)

		h:test("queue preflight reserves lifecycle capacity without poisoning retry", function()
			local statusSolver = makeSolver({ StatusDefinitions.burn() }, { maxQueuedOperations = 2 })
			local input = applyInput("reserved_retry", "burn", 0)
			local canQueue, preflightReason = statusSolver:canQueueApply(input)
			h:assertEqual(canQueue, false)
			h:assertEqual(preflightReason, "QueueFull")
			local queued, queueReason = statusSolver:queueApply(input)
			h:assertEqual(queued, false)
			h:assertEqual(queueReason, "QueueFull")
			statusSolver._maxQueuedOperations = 3
			local retryOk, retryReason = statusSolver:queueApply(input)
			h:assertTrue(retryOk)
			h:assertNil(retryReason)
			local applied = statusSolver:advance(0)
			h:assertTrue(fixtures.findEvent(applied.events, "StatusApplied") ~= nil)
			h:assertNil(fixtures.findEvent(applied.events, "StatusRejected"))

			local driftSolver = makeSolver({ StatusDefinitions.poison() })
			driftSolver:queueApply(applyInput("drift_boundary", "poison", 0.5))
			local driftedTime = 0
			for _ = 1, 10 do
				driftedTime += 0.05
			end
			local driftApplied = driftSolver:advance(driftedTime)
			local driftEvent = fixtures.findEvent(driftApplied.events, "StatusApplied")
			h:assertTrue(driftEvent ~= nil)
			h:assertNear(driftEvent.scheduledTime, 0.5)
			h:assertTrue(driftEvent.processedAt >= driftEvent.scheduledTime)
		end)

		h:test("tick cap leaves bounded work pending", function()
			local definition = fixtures.statusDefinition(Components, "tick_cap", "RefreshDuration", {
				duration = 3,
				tickInterval = 1,
				tickOnApply = true,
				tickAtExpiry = true,
				periodicEffects = { Components.Effect.Damage(1) },
			})
			local statusSolver = makeSolver({ definition }, { maxStatusTicksPerAdvance = 1 })
			statusSolver:queueApply(applyInput("tick_cap_apply", "tick_cap", 0))
			local first = statusSolver:advance(3)
			h:assertEqual(first.ticksProcessed, 1)
			h:assertTrue(first.pendingOperations > 0)
			local second = statusSolver:advance(3)
			h:assertEqual(second.ticksProcessed, 1)
		end)

		h:test("periodic effect budget defers an entire tick instead of applying part", function()
			local definition = fixtures.statusDefinition(Components, "atomic_tick", "RefreshDuration", {
				duration = 1,
				tickInterval = 1,
				tickOnApply = true,
				periodicEffects = {
					Components.Effect.Damage(2),
					Components.Effect.Heal(1),
				},
			})
			local statusSolver, actors = makeSolver({ definition }, {
				maxStatusTicksPerAdvance = 4,
				maxEffectCommandsPerAdvance = 1,
			})
			statusSolver:queueApply(applyInput("atomic_tick_apply", "atomic_tick", 0))
			local blocked = statusSolver:advance(0)
			h:assertEqual(blocked.ticksProcessed, 0)
			h:assertEqual(blocked.processedEffects, 0)
			h:assertNil(fixtures.findEvent(blocked.events, "StatusTick"))
			h:assertNil(fixtures.findEvent(blocked.events, "DamageApplied"))
			h:assertNil(fixtures.findEvent(blocked.events, "HealApplied"))
			h:assertEqual(actors.target.health, 100)
			h:assertTrue(blocked.pendingOperations > 0)

			local completed = statusSolver:advance(0, nil, 2)
			h:assertEqual(completed.ticksProcessed, 1)
			h:assertEqual(completed.processedEffects, 2)
			h:assertTrue(fixtures.findEvent(completed.events, "StatusTick") ~= nil)
			h:assertTrue(fixtures.findEvent(completed.events, "DamageApplied") ~= nil)
			h:assertTrue(fixtures.findEvent(completed.events, "HealApplied") ~= nil)
			h:assertEqual(actors.target.health, 99)
		end)

		h:test("remove path is idempotent and preserves stable instance ID", function()
			local statusSolver = makeSolver({ StatusDefinitions.burn() })
			statusSolver:queueApply(applyInput("remove_apply", "burn", 0))
			statusSolver:advance(0)
			local instanceId = statusSolver:getStatuses("target")[1].instanceId
			statusSolver:queueRemove({
				operationId = "remove_once",
				castId = "cast.status",
				sourceActorId = "source",
				targetActorId = "target",
				scheduledTime = 1,
				instanceId = instanceId,
			})
			local result = statusSolver:advance(1)
			h:assertEqual(fixtures.findEvent(result.events, "StatusRemoved").instanceId, instanceId)
			h:assertEqual(#statusSolver:getStatuses("target"), 0)
		end)
	end)
end
