--!strict

local function blueprint(motion: any, lifetime: number?, emitters: { any }?, timeline: { any }?, burst: any?): any
	local projectile = {
		lifetime = { seconds = lifetime or 1 },
		motion = motion,
		presentation = {
			particles = emitters or {},
			timeline = timeline or {},
		},
	}
	return {
		projectileBlueprint = projectile,
		timeline = timeline or {},
		burst = burst,
	}
end

local function solver(core: any): any
	return core.ProjectileSolver.new({
		eventSolver = {
			stepSeconds = 0.1,
			maxCatchUpSteps = 100,
			maxBacklogSeconds = 10,
		},
	})
end

local function spawn(runtime: any, compiled: any): number
	return runtime:Spawn(compiled, {
		origin = { x = 0, y = 0, z = 0 },
		direction = { x = 0, y = 0, z = -1 },
		rootSeed = 123,
	})
end

return function(harness: any, core: any)
	local ProjectileSolver = core.ProjectileSolver
	local VisualLifecycle = core.VisualLifecycle

	harness:suite("projectile solver", function(h: any)
		h:test("linear motion samples analytically through expiry", function()
			local runtime = solver(core)
			local id = spawn(runtime, blueprint({
				kind = "Motion.Linear",
				velocity = { x = 0, y = 0, z = -10 },
				space = "WorldSnapshot",
			}))
			runtime:Advance(0.5)
			h:assertNear(runtime:GetEntity(id).position.z, -5)
			local result = runtime:Advance(0.5)
			h:assertNear(runtime:GetEntity(id).position.z, -10)
			h:assertEqual(runtime:GetEntity(id).logicalState, "Expired")
			h:assertEqual(runtime:GetEntity(id).visualState, "Released")
			local expired = false
			for _, event in ipairs(result.events) do
				if event.kind == "ProjectileExpired" then
					expired = true
					h:assertNear(event.scheduledTime, 1)
				end
			end
			h:assertTrue(expired)
		end)

		h:test("ballistic motion uses the analytic acceleration term", function()
			local runtime = solver(core)
			local id = spawn(runtime, blueprint({
				kind = "Motion.Ballistic",
				velocity = { x = 10, y = 10, z = 0 },
				acceleration = { x = 0, y = -10, z = 0 },
				velocitySpace = "WorldSnapshot",
			}))
			runtime:Advance(1)
			local position = runtime:GetEntity(id).position
			h:assertNear(position.x, 10)
			h:assertNear(position.y, 5)
			h:assertNear(position.z, 0)
		end)

		h:test("Bezier motion reaches the cubic midpoint and endpoint", function()
			local runtime = solver(core)
			local id = spawn(runtime, blueprint({
				kind = "Motion.Bezier",
				duration = 1,
				space = "WorldSnapshot",
				points = {
					{ x = 0, y = 0, z = 0 },
					{ x = 0, y = 0, z = 0 },
					{ x = 10, y = 0, z = 0 },
					{ x = 10, y = 0, z = 0 },
				},
			}))
			runtime:Advance(0.5)
			h:assertNear(runtime:GetEntity(id).position.x, 5)
			runtime:Advance(0.5)
			h:assertNear(runtime:GetEntity(id).position.x, 10)
		end)

		h:test("CancelAt freezes motion at the trusted scheduled time", function()
			local runtime = solver(core)
			local id = spawn(runtime, blueprint({
				kind = "Motion.Linear",
				velocity = { x = 4, y = 0, z = 0 },
				space = "WorldSnapshot",
			}, 2))
			runtime:Advance(0.3)
			h:assertTrue(runtime:CancelAt(id, 0.25, "Impact"))
			local entity = runtime:GetEntity(id)
			h:assertEqual(entity.logicalState, "Cancelled")
			h:assertNear(entity.position.x, 1)
			h:assertNear(entity.logicalEndTime, 0.25)
			h:assertEqual(entity.cancelReason, "Impact")

			local driftRuntime = solver(core)
			local driftId = spawn(driftRuntime, blueprint({
				kind = "Motion.Linear",
				velocity = { x = 2, y = 0, z = 0 },
				space = "WorldSnapshot",
			}, 2))
			for _ = 1, 10 do
				driftRuntime:Advance(0.05)
			end
			h:assertTrue(driftRuntime:CancelAt(driftId, 0.5, "DriftBoundary"))
			h:assertNear(driftRuntime:GetEntity(driftId).logicalEndTime, 0.5)
		end)

		h:test("CancelAt rejects future pre-spawn and post-end times without state changes", function()
			local runtime = solver(core)
			runtime:Advance(0.5)
			local id = spawn(runtime, blueprint({
				kind = "Motion.Linear",
				velocity = { x = 4, y = 0, z = 0 },
				space = "WorldSnapshot",
			}, 1))

			local active = runtime:GetEntity(id)
			h:assertEqual(runtime:CancelAt(id, 0.6, "future"), false)
			h:assertDeepEqual(runtime:GetEntity(id), active)
			h:assertEqual(runtime:CancelAt(id, 0.4, "pre-spawn"), false)
			h:assertDeepEqual(runtime:GetEntity(id), active)

			runtime:Advance(1)
			local ended = runtime:GetEntity(id)
			h:assertEqual(ended.logicalState, "Expired")
			h:assertEqual(ended.visualState, "Released")
			h:assertEqual(runtime:CancelAt(id, ended.logicalEndTime, "post-end"), false)
			h:assertDeepEqual(runtime:GetEntity(id), ended)
		end)

		h:test("timeline cues retain scheduled time when processed late", function()
			local runtime = solver(core)
			local cue = {
				kind = "Timeline.EmitParticles",
				id = "emit",
				time = 0.15,
				emitterId = "burst",
				count = 3,
			}
			spawn(runtime, blueprint({
				kind = "Motion.Linear",
				velocity = { x = 0, y = 0, z = 0 },
				space = "WorldSnapshot",
			}, 1, {
				{ id = "burst", enabledSeconds = 0, lifetime = { min = 0, max = 0.4 } },
			}, { cue }))
			local result = runtime:Advance(0.2)
			local found = false
			for _, event in ipairs(result.events) do
				if event.kind == "TimelineCue" then
					found = true
					h:assertNear(event.scheduledTime, 0.15)
					h:assertNear(event.processedAt, 0.2)
					h:assertEqual(event.timelineCue.count, 3)
				end
			end
			h:assertTrue(found)
		end)

		h:test("burst child records keep stable index and recipe", function()
			local runtime = solver(core)
			spawn(runtime, blueprint({
				kind = "Motion.Linear",
				velocity = { x = 0, y = 0, z = 0 },
				space = "WorldSnapshot",
			}, 1, {}, {}, {
				trigger = "Expire",
				distribution = "Sphere",
				children = { { id = "pair", recipeId = "child.recipe", count = 2 } },
				maxDepth = 1,
			}))
			local result = runtime:Advance(1)
			local burstEvents = {}
			for _, event in ipairs(result.events) do
				if event.kind == "BurstScheduled" then
					table.insert(burstEvents, event)
				end
			end
			h:assertEqual(#burstEvents, 2)
			h:assertEqual(burstEvents[1].burstChildIndex, 1)
			h:assertEqual(burstEvents[2].burstChildIndex, 2)
			h:assertEqual(burstEvents[2].burstRecipeId, "child.recipe")
			h:assertEqual(burstEvents[1].rootSeed, 123)
		end)

		h:test("event history is bounded", function()
			local runtime = ProjectileSolver.new({
				maxEventHistory = 2,
				eventSolver = { stepSeconds = 0.1, maxCatchUpSteps = 20, maxBacklogSeconds = 2 },
			})
			spawn(runtime, blueprint({
				kind = "Motion.Linear",
				velocity = { x = 0, y = 0, z = 0 },
				space = "WorldSnapshot",
			}))
			local result = runtime:Advance(1)
			h:assertTrue(result.droppedEventHistory > 0)
			h:assertEqual(#runtime:GetEvents(), 2)
		end)

		h:test("released entities leave active work while bounded tombstones remain queryable", function()
			local runtime = ProjectileSolver.new({
				maxEntityTombstones = 1,
				eventSolver = { stepSeconds = 0.1, maxCatchUpSteps = 20, maxBacklogSeconds = 2 },
			})
			local motion = {
				kind = "Motion.Linear",
				velocity = { x = 0, y = 0, z = 0 },
				space = "WorldSnapshot",
			}
			local firstId = spawn(runtime, blueprint(motion, 0.1))
			runtime:Advance(0.1)
			local firstTombstone = runtime:GetEntity(firstId)
			h:assertEqual(firstTombstone.visualState, "Released")
			h:assertEqual(runtime:CancelAt(firstId, 0.05, "too late"), false)
			local idle = runtime:Advance(0.1)
			h:assertEqual(#idle.events, 0)
			h:assertDeepEqual(runtime:GetEntity(firstId), firstTombstone)

			local secondId = spawn(runtime, blueprint(motion, 0.1))
			runtime:Advance(0.1)
			h:assertNil(runtime:GetEntity(firstId))
			h:assertEqual(runtime:GetEntity(secondId).visualState, "Released")
		end)
	end)

	harness:suite("data visual lifecycle", function(h: any)
		h:test("disable and latest emission both extend the tail", function()
			local tracker = VisualLifecycle.new({
				{ id = "trail", lifetime = { max = 1.5 } },
			})
			h:assertTrue(tracker:RecordEmission("trail", 2))
			h:assertTrue(tracker:DisableEmitter("trail", 1))
			local releaseAt = tracker:RequestRelease(2.25)
			h:assertNear(releaseAt, 3.5)
			h:assertEqual(tracker:CanRelease(3.49), false)
			h:assertTrue(tracker:CanRelease(3.5))
		end)

		h:test("cancellation disables only remaining emitters once", function()
			local tracker = VisualLifecycle.new({
				{ id = "early", lifetimeMax = 1 },
				{ id = "late", lifetimeMax = 2 },
			})
			tracker:DisableEmitter("early", 0.5)
			local releaseAt, disabled = tracker:RequestCancel(1)
			h:assertDeepEqual(disabled, { "late" })
			h:assertNear(releaseAt, 3)
			local _, again = tracker:RequestCancel(1.5)
			h:assertEqual(#again, 0)
		end)

		h:test("cancel after emission from an already-disabled emitter waits for emission", function()
			local runtime = solver(core)
			local id = spawn(runtime, blueprint({
				kind = "Motion.Linear",
				velocity = { x = 0, y = 0, z = 0 },
				space = "WorldSnapshot",
			}, 2, {
				{ id = "one_shot", enabledSeconds = 0, lifetime = { min = 0.5, max = 1 } },
			}, {
				{
					kind = "Timeline.EmitParticles",
					id = "late_emit",
					time = 0.2,
					emitterId = "one_shot",
					count = 5,
				},
			}))
			runtime:Advance(0.2)
			runtime:Advance(0.1)
			h:assertTrue(runtime:CancelAt(id, 0.3, "test"))
			local entity = runtime:GetEntity(id)
			h:assertNear(entity.visualLifecycle.emitters[1].disableAt, 0)
			h:assertNear(entity.visualLifecycle.emitters[1].latestEmitAt, 0.2)
			h:assertNear(entity.releaseAt, 1.2)
		end)
	end)
end
