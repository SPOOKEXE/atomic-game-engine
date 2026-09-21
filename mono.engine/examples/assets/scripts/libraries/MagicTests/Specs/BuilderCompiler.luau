--!strict

local function containsForbiddenGameplayKey(value: any): string?
	if type(value) ~= "table" then
		return nil
	end
	for key, child in pairs(value) do
		if key == "effectTriggers"
			or key == "statusDefinitions"
			or key == "damageType"
			or key == "deltaSpeed"
			or key == "maxDeltaSpeed"
		then
			return tostring(key)
		end
		local nested = containsForbiddenGameplayKey(child)
		if nested ~= nil then
			return nested
		end
	end
	return nil
end

return function(harness: any, core: any)
	local Builder = core.Builder
	local Components = core.Components
	local Compiler = core.Compiler
	local Presets = core.Presets
	local TableUtil = core.TableUtil
	local Validator = core.Validator

	harness:suite("builders and compiler", function(h: any)
		h:test("spell builder returns a fresh complete document", function()
			local first = Builder.new("builder_test")
			local second = Builder.new("builder_test")
			h:assertEqual(first.schemaVersion, 1)
			h:assertEqual(first.revision, 1)
			h:assertEqual(first.projectile.lifetime.kind, "Lifetime.Time")
			first.projectile.lifetime.seconds = 9
			h:assertEqual(second.projectile.lifetime.seconds, 1)
		end)

		h:test("projectile builder copies every input", function()
			local lifetime = Components.Lifetime.Time(2)
			local motion = Components.Motion.Linear({ x = 1, y = 2, z = 3 })
			local presentation = Components.Presentation.Part()
			local projectile = Builder.projectile(lifetime, motion, presentation)
			lifetime.seconds = 7
			motion.velocity.x = 99
			presentation.size.x = 20
			h:assertEqual(projectile.lifetime.seconds, 2)
			h:assertEqual(projectile.motion.velocity.x, 1)
			h:assertEqual(projectile.presentation.size.x, 1)
		end)

		h:test("lifetime and all motion builders expose exact records", function()
			h:assertDeepEqual(Components.Lifetime.Time(1.5), { kind = "Lifetime.Time", seconds = 1.5 })
			h:assertDeepEqual(Components.Motion.Linear({ x = 1, y = 0, z = -2 }), {
				kind = "Motion.Linear",
				velocity = { x = 1, y = 0, z = -2 },
			})
			h:assertEqual(Components.Motion.Ballistic({ x = 1, y = 2, z = 3 }, { x = 0, y = -4, z = 0 }).kind, "Motion.Ballistic")
			local bezier = Components.Motion.Bezier(2, {
				{ x = 0, y = 0, z = 0 },
				{ x = 1, y = 0, z = 0 },
				{ x = 2, y = 0, z = 0 },
				{ x = 3, y = 0, z = 0 },
			})
			h:assertEqual(bezier.space, "LaunchLocal")
			h:assertEqual(#bezier.points, 4)
		end)

		h:test("presentation particle curve and timeline builders expose defaults", function()
			local particle = Components.Presentation.Particle("trail", 1, 0.2, 0.8)
			h:assertEqual(particle.kind, "Presentation.Particle")
			h:assertEqual(particle.rate, 0)
			h:assertDeepEqual(particle.lifetime, { min = 0.2, max = 0.8 })
			local curve = Components.Presentation.Curve("scale", "Scale", "NormalizedLifetime", {
				{ time = 0, value = 0 },
				{ time = 1, value = 1 },
			})
			h:assertEqual(curve.keys[2].value, 1)
			local emit = Components.Timeline.EmitParticles("burst", 0.5, "trail", 7)
			local disable = Components.Timeline.SetEmitterEnabled("off", 1, "trail", false)
			h:assertEqual(emit.count, 7)
			h:assertEqual(disable.enabled, false)
		end)

		h:test("event burst and budget builders keep independent data", function()
			local groups = { Components.Burst.Child("group", "recipe", 2) }
			local burst = Components.Burst.Deterministic(groups)
			groups[1].count = 9
			h:assertEqual(burst.children[1].count, 2)
			h:assertEqual(burst.trigger, "Expire")
			h:assertEqual(burst.random.seedSource, "Server")
			local document = Builder.withEvents(Builder.new("events"), {
				Components.Event.Spawn("spawn", 1),
				Components.Event.Expire("expire", 2, "Terminate"),
			})
			document = Builder.withBudgets(document, { maxProjectiles = 2 })
			h:assertEqual(document.events[2].action, "Terminate")
			h:assertEqual(document.budgets.maxProjectiles, 2)
		end)

		h:test("every effect and status builder returns its public kind", function()
			local effects = {
				Components.Effect.Damage(10),
				Components.Effect.Heal(10),
				Components.Effect.Area("Impact", 4, { Components.Effect.Damage(1) }),
				Components.Effect.Pull("Source", 5, 6),
				Components.Effect.ApplyStatus("burn", 1),
				Components.Effect.Cleanse(),
			}
			local expected = {
				"Effect.Damage",
				"Effect.Heal",
				"Effect.Area",
				"Effect.Pull",
				"Effect.ApplyStatus",
				"Effect.Cleanse",
			}
			for index, effect in ipairs(effects) do
				h:assertEqual(effect.kind, expected[index])
			end
			local definition = Components.Status.Definition("slow", 1)
			local modifier = Components.Status.Modifier("speed", "WalkSpeed", "Multiply", 0.8)
			h:assertEqual(definition.stackPolicy, "RefreshDuration")
			h:assertEqual(modifier.operation, "Multiply")
		end)

		h:test("compile does not mutate source and deeply freezes output", function()
			local source = Presets.get("Fireball")
			local before = TableUtil.deepCopy(source)
			local compiled, diagnostics = Compiler.compile(source)
			h:assertEqual(#diagnostics, 0)
			h:assertDeepEqual(source, before)
			h:assertTrue(compiled ~= nil)
			h:assertTrue(table.isfrozen(compiled))
			h:assertTrue(table.isfrozen(compiled.serverProgram.effectTriggers))
			h:assertTrue(table.isfrozen(compiled.clientProgram.projectile.presentation.particles[1]))
			h:assertThrows(function()
				compiled.serverProgram.kind = "changed"
			end)
		end)

		h:test("server and client programs preserve authority separation", function()
			local compiled = Compiler.compile(Presets.get("Fireball"))
			h:assertTrue(compiled ~= nil)
			h:assertNil(compiled.serverProgram.projectile.presentation)
			h:assertTrue(#compiled.serverProgram.effectTriggers > 0)
			h:assertTrue(#compiled.serverProgram.statusDefinitions > 0)
			h:assertNil(compiled.clientProgram.effectTriggers)
			h:assertNil(compiled.clientProgram.statusDefinitions)
			h:assertNil(containsForbiddenGameplayKey(compiled.clientProgram))
			h:assertTrue(#compiled.clientProgram.projectile.presentation.particles > 0)
			local descriptor = compiled.clientProgram.effectCues[1]
			h:assertDeepEqual(descriptor.triggerCueIds, { "spell.fireball.impact" })
			h:assertDeepEqual(descriptor.allowedStatusCueIds, { "status.burn" })
			h:assertNil(descriptor.cueIds)
		end)

		h:test("compile output is stable across map insertion order", function()
			local first = Presets.get("ArcaneBolt")
			local second = Presets.get("ArcaneBolt")
			first.budgets = {
				maxProjectiles = 1,
				maxEvents = 16,
				maxParticles = 128,
				maxBurstDepth = 1,
				maxEffectCommands = 4,
				maxAreaTargets = 1,
				maxStatusesPerActor = 1,
				maxStatusTicksPerAdvance = 1,
			}
			local reordered = {}
			reordered.maxStatusTicksPerAdvance = 1
			reordered.maxStatusesPerActor = 1
			reordered.maxAreaTargets = 1
			reordered.maxEffectCommands = 4
			reordered.maxBurstDepth = 1
			reordered.maxParticles = 128
			reordered.maxEvents = 16
			reordered.maxProjectiles = 1
			second.budgets = reordered
			local firstCompiled = Compiler.compile(first)
			local secondCompiled = Compiler.compile(second)
			h:assertDeepEqual(firstCompiled, secondCompiled)
		end)

		for _, presetName in ipairs(Presets.names()) do
			h:test("preset validates and compiles: " .. presetName, function()
				local document = Presets.get(presetName)
				local before = TableUtil.deepCopy(document)
				local valid, diagnostics = Validator.validate(document)
				h:assertTrue(valid, presetName .. " should validate")
				h:assertEqual(#diagnostics, 0)
				local compiled = Compiler.compile(document)
				h:assertTrue(compiled ~= nil)
				h:assertDeepEqual(document, before)
			end)
		end

		h:test("preset registry returns independent documents", function()
			local first = Presets.get("ArcaneBolt")
			local second = Presets.get("ArcaneBolt")
			first.projectile.lifetime.seconds = 99
			h:assertEqual(second.projectile.lifetime.seconds, 1.25)
			local allFirst = Presets.all()
			local allSecond = Presets.all()
			allFirst.Fireball.id = "changed"
			h:assertEqual(allSecond.Fireball.id, "fireball")
		end)

		local function hasCode(diagnostics: { any }, code: string): boolean
			for _, diagnostic in ipairs(diagnostics) do
				if diagnostic.code == code then return true end
			end
			return false
		end

		local function graphDocument(id: string, children: { any }?, rootDepth: number?): any
			local document = Presets.get("ArcaneBolt")
			document.id = id
			document.budgets.maxProjectiles = 4096
			document.budgets.maxEvents = 4096
			document.budgets.maxParticles = 100000
			document.budgets.maxEffectCommands = 100000
			document.budgets.maxAreaTargets = 128
			document.budgets.maxStatusesPerActor = 128
			document.budgets.maxStatusTicksPerAdvance = 4096
			document.budgets.maxBurstDepth = rootDepth or 4
			if children ~= nil then
				local burstSource = Presets.get("MeteorCluster")
				document.burst = burstSource.burst
				document.burst.children = children
				document.burst.maxDepth = 1
				document.events = burstSource.events
			end
			return document
		end

		h:test("preset graphs are fresh and Meteor owns both child documents", function()
			local first = Presets.getGraph("MeteorCluster")
			local second = Presets.getGraph("MeteorCluster")
			h:assertEqual(first.recipes.meteor_child_molten.id, "meteor_child_molten")
			h:assertEqual(first.recipes.meteor_child_cinder.id, "meteor_child_cinder")
			h:assertNil(first.recipes.meteor_child_molten.burst)
			first.recipes.meteor_child_molten.id = "changed"
			h:assertEqual(second.recipes.meteor_child_molten.id, "meteor_child_molten")
			h:assertNil(Presets.getGraph("Fireball").recipes.fireball)
		end)

		h:test("graph reports missing direct and transitive recipes with graph paths", function()
			local _, direct = Compiler.compileGraph(graphDocument("root", { Components.Burst.Child("x", "missing", 1) }), {})
			h:assertTrue(hasCode(direct, "MISSING_RECIPE"))
			h:assertTrue(string.find(direct[1].path, "recipeId", 1, true) ~= nil)
			local child = graphDocument("child", { Components.Burst.Child("x", "missing_leaf", 1) })
			local _, transitive = Compiler.compileGraph(graphDocument("root", { Components.Burst.Child("x", "child", 1) }), { child = child })
			h:assertTrue(hasCode(transitive, "MISSING_RECIPE"))
	end)

		h:test("graph reports recipe id mismatch and ignores unreachable bad recipes", function()
			local root = graphDocument("root", { Components.Burst.Child("x", "child", 1) })
			local wrong = graphDocument("other")
			local _, mismatch = Compiler.compileGraph(root, { child = wrong })
			h:assertTrue(hasCode(mismatch, "RECIPE_ID_MISMATCH"))
			local graph, diagnostics = Compiler.compileGraph(graphDocument("clean"), { bad = { id = "wrong" } })
			h:assertTrue(graph ~= nil)
			h:assertEqual(#diagnostics, 0)
			h:assertNil(graph.recipes.bad)
	end)

		h:test("graph rejects self and multi-recipe cycles but permits a shared DAG", function()
			local rootEdge = graphDocument("root", { Components.Burst.Child("back", "root", 1) })
			local _, rootDiagnostics = Compiler.compileGraph(rootEdge, {})
			h:assertTrue(hasCode(rootDiagnostics, "RECIPE_CYCLE"))
			h:assertTrue(not hasCode(rootDiagnostics, "MISSING_RECIPE"))
			local selfRecipe = graphDocument("self", { Components.Burst.Child("again", "self", 1) })
			local _, selfDiagnostics = Compiler.compileGraph(graphDocument("root", { Components.Burst.Child("x", "self", 1) }), { self = selfRecipe })
			h:assertTrue(hasCode(selfDiagnostics, "RECIPE_CYCLE"))
			local a = graphDocument("a", { Components.Burst.Child("to_b", "b", 1) })
			local b = graphDocument("b", { Components.Burst.Child("to_a", "a", 1) })
			local _, multi = Compiler.compileGraph(graphDocument("root", { Components.Burst.Child("x", "a", 1) }), { a = a, b = b })
			h:assertTrue(hasCode(multi, "RECIPE_CYCLE"))
			local leaf = graphDocument("leaf")
			local left = graphDocument("left", { Components.Burst.Child("leaf", "leaf", 1) })
			local right = graphDocument("right", { Components.Burst.Child("leaf", "leaf", 1) })
			local dag = Compiler.compileGraph(graphDocument("root", {
				Components.Burst.Child("left", "left", 1), Components.Burst.Child("right", "right", 1),
			}), { left = left, right = right, leaf = leaf })
			h:assertTrue(dag ~= nil)
	end)

		h:test("graph accepts exact root depth and rejects one level over", function()
			local leaf = graphDocument("leaf")
			local child = graphDocument("child", { Components.Burst.Child("leaf", "leaf", 1) })
			local exact = Compiler.compileGraph(graphDocument("root", { Components.Burst.Child("child", "child", 1) }, 2), { child = child, leaf = leaf })
			h:assertTrue(exact ~= nil)
			local _, over = Compiler.compileGraph(graphDocument("root", { Components.Burst.Child("child", "child", 1) }, 1), { child = child, leaf = leaf })
			h:assertTrue(hasCode(over, "RECIPE_DEPTH_EXCEEDED"))
	end)

		h:test("graph aggregate costs follow uneven branches and include child work", function()
			local leaf = graphDocument("leaf")
			local branch = graphDocument("branch", { Components.Burst.Child("leaf", "leaf", 3) })
			local root = graphDocument("root", {
				Components.Burst.Child("branch", "branch", 2), Components.Burst.Child("leaf", "leaf", 1),
			})
			local graph = Compiler.compileGraph(root, { branch = branch, leaf = leaf })
			h:assertEqual(graph.kind, "CompiledSpellGraph")
			h:assertEqual(graph.schemaVersion, 1)
			h:assertEqual(graph.compilerVersion, 1)
			h:assertEqual(graph.rootId, graph.root.id)
			h:assertTrue(table.isfrozen(graph))
			h:assertTrue(table.isfrozen(graph.root))
			h:assertTrue(table.isfrozen(graph.recipes))
			local rootLocal = Compiler.compile(root).costs
			local branchLocal = Compiler.compile(branch).costs
			local leafLocal = Compiler.compile(leaf).costs
			for _, field in ipairs({
				"projectiles", "events", "particleEmissions", "particleEmitters", "burstChildren",
				"effectCommands", "areaTargets", "statusInstances", "statusTicks",
			}) do
				local expected = rootLocal[field] + 2 * (branchLocal[field] + 3 * leafLocal[field]) + leafLocal[field]
				h:assertEqual(graph.costs[field], expected, "aggregate mismatch for " .. field)
			end
		end)

		h:test("deep freeze is idempotent so a graph can wrap already-frozen documents", function()
			-- compileGraph freezes a wrapper whose root and recipes compileDocument
			-- already froze. Luau's table.freeze throws on an already-frozen table, so a
			-- naive deep freeze breaks every graph compile.
			local nested = core.TableUtil.deepFreeze({ value = 1 })
			h:assertTrue(table.isfrozen(nested))

			local wrapper = { child = nested, sibling = { value = 2 } }
			h:assertEqual(core.TableUtil.deepFreeze(wrapper), wrapper)
			h:assertTrue(table.isfrozen(wrapper))
			-- A frozen table may still hold a mutable one, so children stay in scope.
			h:assertTrue(table.isfrozen(wrapper.sibling))
		end)

		h:test("area status costs multiply by max targets", function()
			-- Burn runs 6s on a 1s interval with tickAtExpiry, so ordinals 1..6 all fire:
			-- 6 ticks per target across the area's 8 max targets.
			local compiled = Compiler.compile(Presets.get("Fireball"))
			h:assertEqual(compiled.costs.statusInstances, 8)
			h:assertEqual(compiled.costs.statusTicks, 48)
	end)

		h:test("graph detects conflicting reachable status definitions", function()
			local root = Presets.get("Fireball")
			local burstSource = Presets.get("MeteorCluster")
			root.burst = burstSource.burst
			root.burst.children = { Components.Burst.Child("conflict", "conflict", 1) }
			root.burst.maxDepth = 1
			root.events = burstSource.events
			root.budgets.maxProjectiles = 16
			local conflict = Presets.get("Fireball")
			conflict.id = "conflict"
			conflict.statusDefinitions[1].periodicEffects[1].amount = 99
			local _, diagnostics = Compiler.compileGraph(root, { conflict = conflict })
			h:assertTrue(hasCode(diagnostics, "CONFLICTING_STATUS_DEFINITION"))
	end)

		h:test("aggregate graph budgets are rooted and authoritative", function()
			local root = graphDocument("root", { Components.Burst.Child("branch", "branch", 1) }, 2)
			root.budgets.maxProjectiles = 2
			local branch = graphDocument("branch", { Components.Burst.Child("leaf", "leaf", 2) })
			local _, diagnostics = Compiler.compileGraph(root, { branch = branch, leaf = graphDocument("leaf") })
			h:assertTrue(hasCode(diagnostics, "BURST_CAP_EXCEEDED"))
		end)

		h:test("graph projectile validation uses heterogeneous aggregate under root budget", function()
			local root = graphDocument("root", { Components.Burst.Child("branch", "branch", 1) }, 2)
			root.budgets.maxProjectiles = 3
			local branch = graphDocument("branch", { Components.Burst.Child("leaf", "leaf", 1) })
			branch.budgets.maxProjectiles = 1
			local standalone, standaloneDiagnostics = Compiler.compile(branch)
			h:assertNil(standalone)
			h:assertTrue(hasCode(standaloneDiagnostics, "BURST_CAP_EXCEEDED"))
			local graph, diagnostics = Compiler.compileGraph(root, { branch = branch, leaf = graphDocument("leaf") })
			h:assertTrue(graph ~= nil)
			h:assertEqual(#diagnostics, 0)
			h:assertEqual(graph.costs.projectiles, 3)
		end)

		h:test("aggregate status ticks remain informational", function()
			local root = Presets.get("Fireball")
			root.budgets.maxStatusTicksPerAdvance = 1
			local graph, diagnostics = Compiler.compileGraph(root, {})
			h:assertTrue(graph ~= nil)
			h:assertEqual(#diagnostics, 0)
			h:assertEqual(graph.costs.statusTicks, 48)
		end)
	end)
end
