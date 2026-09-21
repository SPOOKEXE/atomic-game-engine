--!strict

local function actors(fixtures: any): { any }
	return {
		fixtures.actor("source", 0, 100, { "caster" }, "blue"),
		fixtures.actor("target", 10, 100, { "enemy" }, "red"),
	}
end

-- A spell whose impact carries an Area effect needs area candidates at cast time.
-- dispatch preflights every effect before it walks the event records, so a missing
-- candidate list rejects the whole trigger, burst children included, and the caller
-- sees no ChildSpawned record at all rather than a ChildSpawnRejected one.
local function cast(solver: any, _compiled: any, candidates: { any }?, castId: string?): number
	local accepted = solver:castTrusted({
		castId = castId or "cast.end_to_end",
		sourceActorId = "source",
		targetActorId = "target",
		origin = { x = 0, y = 0, z = 0 },
		direction = { x = 0, y = 0, z = -1 },
		rootSeed = 2468,
		areaCandidates = candidates,
	})
	return accepted.entityId
end

return function(harness: any, core: any, fixtures: any)
	local Compiler = core.Compiler
	local Presets = core.Presets
	local SpellSolver = core.SpellSolver
	local TableUtil = core.TableUtil
	local function solverConfig(stepSeconds: number?, maxQueuedStatusOperations: number?): any
		return {
			actors = actors(fixtures),
			projectile = {
				eventSolver = {
					stepSeconds = stepSeconds or 0.05,
					maxCatchUpSteps = 100,
					maxBacklogSeconds = 10,
				},
			},
			maxQueuedStatusOperations = maxQueuedStatusOperations,
		}
	end

	local function newSolver(compiled: any, stepSeconds: number?, maxQueuedStatusOperations: number?): any
		local config = solverConfig(stepSeconds, maxQueuedStatusOperations)
		config.compiledSpell = compiled
		return SpellSolver.new(config)
	end

	local function newGraphSolver(graph: any, stepSeconds: number?, maxQueuedStatusOperations: number?): any
		local config = solverConfig(stepSeconds, maxQueuedStatusOperations)
		config.graph = graph
		return SpellSolver.new(config)
	end

	harness:suite("preset contracts", function(h: any)
		h:test("canonical presets expose the intended modular shapes", function()
			local expected: { [string]: { id: string, motion: string } } = {
				ArcaneBolt = { id = "arcane_bolt", motion = "Motion.Linear" },
				AtomicNova = { id = "atomic_nova", motion = "Motion.Ballistic" },
				Fireball = { id = "fireball", motion = "Motion.Ballistic" },
				GravityWell = { id = "gravity_well", motion = "Motion.Ballistic" },
				HealingBloom = { id = "healing_bloom", motion = "Motion.Ballistic" },
				MeteorCluster = { id = "meteor_cluster", motion = "Motion.Ballistic" },
				ParticleTailShowcase = { id = "particle_tail_showcase", motion = "Motion.Linear" },
				PurifyingPulse = { id = "purifying_pulse", motion = "Motion.Ballistic" },
				VenomDart = { id = "venom_dart", motion = "Motion.Bezier" },
			}
			for name, contract in pairs(expected) do
				local preset = Presets.get(name)
				h:assertEqual(preset.id, contract.id)
				h:assertEqual(preset.projectile.motion.kind, contract.motion)
				h:assertTrue(#preset.events > 0)
			end
			h:assertEqual(#Presets.get("MeteorCluster").burst.children, 2)
			h:assertEqual(#Presets.get("ParticleTailShowcase").projectile.presentation.particles, 4)
			-- Two warhead emitters plus nine cue-driven detonation emitters.
			h:assertEqual(#Presets.get("AtomicNova").projectile.presentation.particles, 11)
		end)

		h:test("legacy preset aliases still return fresh canonical data", function()
			local straight = Presets.get("StraightBolt")
			local arcane = Presets.get("ArcaneBolt")
			h:assertDeepEqual(straight, arcane)
			straight.id = "changed"
			h:assertEqual(Presets.get("StraightBolt").id, "arcane_bolt")
		end)
	end)

	harness:suite("spell solver end to end", function(h: any)
		local function recursiveDocument(id: string, recipeId: string, maxDepth: number, maxProjectiles: number, rootDepth: number?): any
			local document = Presets.get("MeteorCluster")
			document.id = id
			document.burst.children = {
				{ id = id .. "_children", recipeId = recipeId, count = 1 },
			}
			document.burst.maxDepth = maxDepth
			document.budgets.maxBurstDepth = rootDepth or maxDepth
			document.budgets.maxProjectiles = maxProjectiles
			document.budgets.maxEvents = 4096
			document.budgets.maxParticles = 100000
			document.budgets.maxEffectCommands = 100000
			document.budgets.maxAreaTargets = 128
			document.budgets.maxStatusesPerActor = 128
			return document
		end

		local function recipeDocument(presetName: string, id: string): any
			local document = Presets.get(presetName)
			document.id = id
			return document
		end

		local function childRecord(result: any, kind: string): any?
			for _, record in ipairs(result.records) do
				if record.kind == kind then
					return record
				end
			end
			return nil
		end

		h:test("early impact emits its trigger cue then advances queued status at controlled time", function()
			local compiled = Compiler.compile(Presets.get("Fireball"))
			local spellSolver = newSolver(compiled)
			local entityId = cast(spellSolver, compiled, {
				{ id = "target", relationToSource = "Ally" },
			})
			spellSolver:advance(0.5)
			local result = spellSolver:triggerImpact({
				entityId = entityId,
				targetActorId = "target",
				scheduledTime = 0.5,
				processedAt = 0.9,
				position = { x = 10, y = 0, z = 0 },
			})
			local damage = fixtures.findEvent(result.events, "DamageApplied")
			local queued = fixtures.findEvent(result.events, "StatusCommandQueued")
			local rejected = fixtures.findEvent(result.events, "SpellTriggerRejected")
			h:assertNil(rejected, if rejected == nil then nil else rejected.reason)
			h:assertTrue(damage ~= nil, "expected DamageApplied after accepted impact trigger")
			h:assertNear(damage.scheduledTime, 0.5)
			h:assertNear(damage.processedAt, 0.9)
			h:assertEqual(spellSolver:getActor("target").health, 35)
			h:assertTrue(queued ~= nil)
			h:assertNil(fixtures.findEvent(result.events, "StatusApplied"))
			local triggerCueCount = 0
			local earlyStatusCueCount = 0
			local terminationFound = false
			for _, record in ipairs(result.records) do
				h:assertNear(record.scheduledTime, 0.5)
				if record.kind == "PresentationCue" and record.cueId == "spell.fireball.impact" then
					triggerCueCount += 1
				elseif record.kind == "PresentationCue" and record.cueId == "status.burn" then
					earlyStatusCueCount += 1
				elseif record.kind == "Termination" then
					terminationFound = true
				end
			end
			h:assertEqual(triggerCueCount, 1)
			h:assertEqual(earlyStatusCueCount, 0)
			h:assertTrue(terminationFound)
			h:assertEqual(spellSolver:getEntity(entityId).logicalState, "Cancelled")

			local statusAdvance = spellSolver:advance(0)
			h:assertTrue(fixtures.findEvent(statusAdvance.events, "StatusApplied") ~= nil)
			h:assertEqual(#spellSolver:getActor("target").statuses, 1)
			local statusCueCount = 0
			for _, record in ipairs(statusAdvance.records) do
				if record.kind == "PresentationCue" and record.cueId == "status.burn" then
					statusCueCount += 1
				end
			end
			h:assertEqual(statusCueCount, 1)
			local noDuplicate = spellSolver:advance(0)
			for _, record in ipairs(noDuplicate.records) do
				h:assertTrue(record.kind ~= "PresentationCue" or record.cueId ~= "status.burn")
			end
		end)

		h:test("status queue failure leaves trigger health and projectile retryable", function()
			local compiled = Compiler.compile(Presets.get("Fireball"))
			local spellSolver = newSolver(compiled, nil, 1)
			local entityId = cast(spellSolver, compiled, {
				{ id = "target", relationToSource = "Ally" },
			})
			spellSolver:advance(0.5)
			local input = {
				entityId = entityId,
				targetActorId = "target",
				scheduledTime = 0.5,
				processedAt = 0.9,
				position = { x = 10, y = 0, z = 0 },
				operationId = "atomic_status_queue_failure",
			}
			local first = spellSolver:triggerImpact(input)
			local rejected = fixtures.findEvent(first.events, "SpellTriggerRejected")
			h:assertEqual(rejected.reason, "EffectPreflightFailed")
			h:assertEqual(rejected.failureReason, "QueueFull")
			h:assertEqual(first.processedEffects, 0)
			h:assertEqual(#first.commands, 0)
			h:assertEqual(#first.records, 0)
			h:assertEqual(spellSolver:getActor("target").health, 100)
			h:assertEqual(spellSolver:getEntity(entityId).logicalState, "Active")
			h:assertNil(spellSolver._triggerLedger.atomic_status_queue_failure)

			local retry = spellSolver:triggerImpact(input)
			local retryRejected = fixtures.findEvent(retry.events, "SpellTriggerRejected")
			h:assertEqual(retry.duplicate, false)
			h:assertEqual(retryRejected.failureReason, "QueueFull")
			h:assertEqual(spellSolver:getActor("target").health, 100)
			h:assertEqual(spellSolver:getEntity(entityId).logicalState, "Active")
			h:assertNil(spellSolver._triggerLedger.atomic_status_queue_failure)
		end)

		h:test("Meteor graph impact spawns its concrete child recipes", function()
			local documents = Presets.getGraph("MeteorCluster")
			local graph, diagnostics = Compiler.compileGraph(documents.root, documents.recipes)
			h:assertEqual(#diagnostics, 0)
			local spellSolver = newGraphSolver(graph)
			local compiled = graph.root
			local entityId = cast(spellSolver, compiled, {
				{ id = "target", relationToSource = "Ally" },
			})
			spellSolver:advance(0.75)
			local result = spellSolver:triggerImpact({
				entityId = entityId,
				targetActorId = "target",
				scheduledTime = 0.75,
				processedAt = 1,
				position = { x = 10, y = 0, z = 0 },
			})
			local children: { any } = {}
			for _, record in ipairs(result.records) do
				if record.kind == "ChildSpawned" then
					table.insert(children, record)
				end
			end
			h:assertEqual(#children, 5)
			for index, child in ipairs(children) do
				h:assertEqual(child.childIndex, index)
				h:assertNil(child.reason)
				h:assertNear(child.scheduledTime, 0.75)
				local expectedRecipe = if index <= 3 then "meteor_child_molten" else "meteor_child_cinder"
				h:assertEqual(child.recipeId, expectedRecipe)
			end
		end)

		h:test("AtomicNova throws its cluster level around the blast", function()
			local documents = Presets.getGraph("AtomicNova")
			h:assertTrue(documents.recipes.atomic_nova_cluster ~= nil)
			local graph, diagnostics = Compiler.compileGraph(documents.root, documents.recipes)
			h:assertEqual(#diagnostics, 0)

			local spellSolver = newGraphSolver(graph)
			local entityId = cast(spellSolver, graph.root, { "target" })
			spellSolver:advance(0.5)
			local result = spellSolver:triggerImpact({
				entityId = entityId,
				targetActorId = "target",
				scheduledTime = 0.5,
				position = { x = 4, y = 0, z = -2 },
			})

			local children: { any } = {}
			for _, record in ipairs(result.records) do
				if record.kind == "ChildSpawned" then
					table.insert(children, record)
				end
			end
			h:assertEqual(#children, 10)

			local seenAzimuths: { [string]: boolean } = {}
			for index, child in ipairs(children) do
				h:assertEqual(child.recipeId, "atomic_nova_cluster")
				h:assertEqual(child.childIndex, index)
				-- Level launch is the whole point of Ring: it is what makes the child's
				-- authored local +Y resolve to world up, so the recipe's arc decides the
				-- radius instead of the direction landing somewhere on a sphere.
				h:assertNear(child.direction.y, 0)
				h:assertNear(
					math.sqrt(child.direction.x ^ 2 + child.direction.z ^ 2),
					1,
					1e-9,
					"ring directions must be unit length in the horizontal plane"
				)
				local key = string.format("%.6f", math.atan2(child.direction.z, child.direction.x))
				h:assertNil(seenAzimuths[key], "ring children must not share an azimuth")
				seenAzimuths[key] = true

				-- Every submunition starts at the blast point and flies out from there.
				h:assertDeepEqual(spellSolver:getEntity(child.entityId).origin, { x = 4, y = 0, z = -2 })
			end
		end)

		h:test("recursive burst spawns child and grandchild with child-owned spells", function()
			local child = recursiveDocument("recursive_child", "leaf", 2, 4)
			local root = recursiveDocument("recursive_root", "recursive_child", 2, 4)
			local graph = Compiler.compileGraph(root, { recursive_child = child, leaf = recipeDocument("ArcaneBolt", "leaf") })
			local spellSolver = newGraphSolver(graph)
			local rootId = cast(spellSolver, graph.root, { "target" })
			spellSolver:advance(0.1)
			local first = spellSolver:triggerImpact({
				entityId = rootId,
				scheduledTime = 0.1,
				position = { x = 1, y = 2, z = 3 },
			})
			local childSpawn = childRecord(first, "ChildSpawned")
			h:assertTrue(childSpawn ~= nil)
			h:assertEqual(childSpawn.depth, 1)
			h:assertDeepEqual(spellSolver:getEntity(childSpawn.entityId).origin, { x = 1, y = 2, z = 3 })

			local second = spellSolver:triggerImpact({
				entityId = childSpawn.entityId,
				scheduledTime = 0.1,
				position = { x = 4, y = 5, z = 6 },
			})
			local grandchildSpawn = childRecord(second, "ChildSpawned")
			h:assertTrue(grandchildSpawn ~= nil, "child dispatch must use the child's burst program")
			h:assertEqual(grandchildSpawn.depth, 2)
			h:assertDeepEqual(spellSolver:getEntity(grandchildSpawn.entityId).origin, { x = 4, y = 5, z = 6 })
		end)

		h:test("child recipe burst depth is local while root depth stays absolute", function()
			local child = recursiveDocument("local_depth_child", "leaf", 1, 3)
			local root = recursiveDocument("absolute_depth_root", "local_depth_child", 1, 3, 2)
			local graph = Compiler.compileGraph(root, { local_depth_child = child, leaf = recipeDocument("ArcaneBolt", "leaf") })
			local spellSolver = newGraphSolver(graph)
			local rootId = cast(spellSolver, graph.root, { "target" })
			spellSolver:advance(0.1)
			local first = spellSolver:triggerImpact({ entityId = rootId, scheduledTime = 0.1, position = { x = 0, y = 0, z = 0 } })
			local spawned = childRecord(first, "ChildSpawned")
			local second = spellSolver:triggerImpact({ entityId = spawned.entityId, scheduledTime = 0.1, position = { x = 0, y = 0, z = 0 } })
			local grandchild = childRecord(second, "ChildSpawned")
			h:assertTrue(grandchild ~= nil)
			h:assertEqual(grandchild.depth, 2)
		end)

		h:test("recipe status definitions are available to child effects", function()
			local child = recipeDocument("Fireball", "child")
			local root = recursiveDocument("status_recipe_root", "child", 1, 2)
			local graph = Compiler.compileGraph(root, { child = child })
			local spellSolver = newGraphSolver(graph)
			local rootId = cast(spellSolver, graph.root, { "target" })
			spellSolver:advance(0.1)
			local first = spellSolver:triggerImpact({ entityId = rootId, scheduledTime = 0.1, position = { x = 0, y = 0, z = 0 } })
			local spawned = childRecord(first, "ChildSpawned")
			local second = spellSolver:triggerImpact({
				entityId = spawned.entityId,
				targetActorId = "target",
				scheduledTime = 0.1,
				position = { x = 0, y = 0, z = 0 },
			})
			h:assertTrue(fixtures.findEvent(second.events, "StatusCommandQueued") ~= nil)
		end)

		h:test("recursive burst seed and direction repeat deterministically", function()
			local child = recipeDocument("ArcaneBolt", "child")
			local document = Presets.get("MeteorCluster")
			document.id = "deterministic_root"
			document.burst.children = {
				{ id = "deterministic_children", recipeId = "child", count = 2 },
			}
			document.budgets.maxProjectiles = 3
			local graph = Compiler.compileGraph(document, { child = child })
			local launches = {}
			for run = 1, 2 do
				local spellSolver = newGraphSolver(graph)
				local rootId = cast(spellSolver, graph.root, { "target" })
				spellSolver:advance(0.1)
				local result = spellSolver:triggerImpact({
					entityId = rootId,
					scheduledTime = 0.1,
					position = { x = 0, y = 0, z = 0 },
				})
				launches[run] = {}
				for _, record in ipairs(result.records) do
					if record.kind == "ChildSpawned" then
						table.insert(launches[run], { seed = record.seed, direction = record.direction })
					end
				end
			end
			h:assertEqual(#launches[1], 2)
			h:assertDeepEqual(launches[1], launches[2])
			h:assertTrue(launches[1][1].seed ~= launches[1][2].seed)
			local firstDirection = launches[1][1].direction
			local secondDirection = launches[1][2].direction
			h:assertTrue(
				firstDirection.x ~= secondDirection.x
					or firstDirection.y ~= secondDirection.y
					or firstDirection.z ~= secondDirection.z
			)
		end)

		h:test("compiled graph rejects recipes beyond root burst depth", function()
			local child = recursiveDocument("depth_child", "leaf", 2, 4)
			local root = recursiveDocument("depth_root", "depth_child", 1, 4)
			local graph, diagnostics = Compiler.compileGraph(root, {
				depth_child = child,
				leaf = recipeDocument("ArcaneBolt", "leaf"),
			})
			h:assertNil(graph)
			h:assertEqual(diagnostics[1].code, "RECIPE_DEPTH_EXCEEDED")
		end)

		h:test("compiled graph enforces root projectile cap across the cast tree", function()
			local document = Presets.get("MeteorCluster")
			document.burst.children = {
				{ id = "cap_children", recipeId = "leaf", count = 2 },
			}
			document.budgets.maxProjectiles = 2
			local graph, diagnostics = Compiler.compileGraph(document, { leaf = recipeDocument("ArcaneBolt", "leaf") })
			h:assertNil(graph)
			h:assertEqual(diagnostics[1].code, "BURST_CAP_EXCEEDED")
		end)

		h:test("expire dispatches area pull and termination with late diagnostics", function()
			local compiled = Compiler.compile(Presets.get("GravityWell"))
			local spellSolver = newSolver(compiled, 1)
			cast(spellSolver, compiled, {
				{ id = "target", relationToSource = "Ally" },
			})
			local result = spellSolver:advance(3)
			h:assertEqual(#result.commands, 1)
			h:assertEqual(result.commands[1].kind, "PullCommand")
			h:assertNear(result.commands[1].scheduledTime, 2.5)
			h:assertNear(result.commands[1].processedAt, 3)
			local termination: any = nil
			for _, record in ipairs(result.records) do
				if record.kind == "Termination" then
					termination = record
					break
				end
			end
			h:assertTrue(termination ~= nil)
			h:assertEqual(termination.kind, "Termination")
			h:assertNear(termination.scheduledTime, 2.5)
		end)

		h:test("trusted trigger operation is idempotent", function()
			local compiled = Compiler.compile(Presets.get("ArcaneBolt"))
			local spellSolver = newSolver(compiled)
			local entityId = cast(spellSolver, compiled)
			spellSolver:advance(0.1)
			local input = {
				entityId = entityId,
				targetActorId = "target",
				scheduledTime = 0.1,
				operationId = "same_trigger",
			}
			spellSolver:triggerImpact(input)
			local duplicate = spellSolver:triggerImpact(input)
			h:assertTrue(duplicate.duplicate)
			h:assertEqual(duplicate.events[1].kind, "SpellTriggerDuplicateIgnored")
			h:assertEqual(spellSolver:getActor("target").health, 62)
		end)

		h:test("trusted cast rejects per-cast compiled spell", function()
			local fixedCompiled = Compiler.compile(Presets.get("ArcaneBolt"))
			local overrideCompiled = Compiler.compile(Presets.get("Fireball"))
			local spellSolver = newSolver(fixedCompiled)
			h:assertThrows(function()
				spellSolver:castTrusted({ castId = "override", sourceActorId = "source", origin = { x = 0, y = 0, z = 0 }, direction = { x = 0, y = 0, z = -1 }, rootSeed = 1, compiledSpell = overrideCompiled })
			end, "compiledSpell is not accepted")
		end)

		h:test("graph construction rejects hand-built and drifted bundles", function()
			local documents = Presets.getGraph("MeteorCluster")
			local graph = Compiler.compileGraph(documents.root, documents.recipes)
			h:assertThrows(function()
				SpellSolver.new({ graph = { rootId = graph.rootId, root = graph.root, recipes = graph.recipes, costs = graph.costs } })
			end, "compiler-produced")

			local unfrozen = TableUtil.deepCopy(graph)
			h:assertThrows(function()
				SpellSolver.new({ graph = unfrozen })
			end, "graph must be frozen")

			local wrongVersion = TableUtil.deepCopy(graph)
			wrongVersion.compilerVersion += 1
			TableUtil.deepFreeze(wrongVersion)
			h:assertThrows(function()
				SpellSolver.new({ graph = wrongVersion })
			end, "compilerVersion is unsupported")

			local wrongRoot = TableUtil.deepCopy(graph)
			wrongRoot.rootId = "wrong"
			TableUtil.deepFreeze(wrongRoot)
			h:assertThrows(function()
				SpellSolver.new({ graph = wrongRoot })
			end, "rootId must match")
		end)

		h:test("trusted cast rejects malformed authority fields and duplicate cast ids", function()
			local compiled = Compiler.compile(Presets.get("ArcaneBolt"))
			local function fresh(): any
				return newSolver(compiled)
			end
			local valid = {
				castId = "boundary", sourceActorId = "source", origin = { x = 0, y = 0, z = 0 },
				direction = { x = 0, y = 0, z = -1 }, rootSeed = 1,
			}
			local badInputs: { any } = {
				{ field = "origin", value = { x = math.huge, y = 0, z = 0 }, message = "origin must be a finite vector" },
				{ field = "direction", value = { x = 0, y = 0, z = 0 }, message = "direction must be non-zero" },
				{ field = "direction", value = { x = 0 / 0, y = 0, z = 1 }, message = "direction must be a finite vector" },
				{ field = "rootSeed", value = 1.5, message = "rootSeed must be a finite safe integer" },
				{ field = "rootSeed", value = 9007199254740992, message = "rootSeed must be a finite safe integer" },
				{ field = "sourceActorId", value = "missing", message = "known actor" },
			}
			for _, bad in ipairs(badInputs) do
				local input = table.clone(valid)
				input[bad.field] = bad.value
				h:assertThrows(function()
					fresh():castTrusted(input)
				end, bad.message)
			end
			local solver = fresh()
			solver:castTrusted(valid)
			h:assertThrows(function()
				solver:castTrusted(valid)
			end, "castId has already been used")
		end)
	end)
end
