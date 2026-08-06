--!strict

-- The `Arc` burst distribution and the recursive chain built on it.
--
-- Both are new surface, and both are the kind that passes a smoke test while
-- being wrong: a chain compiles and casts whether or not its hops actually turn,
-- and the depth machinery had never been exercised by content at all, so
-- "ChainLightning spawns something" proves very little. These assert the two
-- properties that make it a chain -- every hop deviates, and the chain reaches
-- its authored length and then stops.

return function(harness: any, core: any)
	local Compiler = core.Compiler
	local Enums = core.Enums
	local Presets = core.Presets
	local Validator = core.Validator
	local Builder = core.Builder
	local TableUtil = core.TableUtil

	local function dot(a: any, b: any): number
		return a.x * b.x + a.y * b.y + a.z * b.z
	end

	harness:suite("arc burst distribution", function(h: any)
		h:test("an arc burst requires both bounds", function()
			local document = Presets.get("ChainLightning")
			document.burst.arcMinDegrees = nil
			local _, diagnostics = Validator.validate(document)
			h:assertDiagnostic(diagnostics, "MISSING_FIELD", "$.burst.arcMinDegrees")
		end)

		h:test("an inverted arc band is refused", function()
			-- An inverted band would draw from a range whose ends cross, so every
			-- child leaves along one edge and the chain stops turning. That is a
			-- data bug that looks like a physics bug, which is why it is refused
			-- at authoring time rather than diagnosed by eye.
			local document = Presets.get("ChainLightning")
			document.burst.arcMinDegrees = 90
			document.burst.arcMaxDegrees = 30
			local _, diagnostics = Validator.validate(document)
			h:assertDiagnostic(diagnostics, "INVALID_ARC_RANGE", "$.burst.arcMinDegrees")
		end)

		h:test("arc bounds are refused on other distributions", function()
			local document = Presets.get("MeteorCluster")
			document.burst.arcMinDegrees = 10
			local _, diagnostics = Validator.validate(document)
			h:assertDiagnostic(diagnostics, "FIELD_NOT_ALLOWED", "$.burst.arcMinDegrees")
		end)

		h:test("a sphere burst still refuses cone and arc fields together", function()
			local document = Presets.get("ChainLightning")
			document.burst.distribution = Enums.BurstDistribution.Sphere
			local _, diagnostics = Validator.validate(document)
			h:assertDiagnostic(diagnostics, "FIELD_NOT_ALLOWED", "$.burst.arcMinDegrees")
			h:assertDiagnostic(diagnostics, "FIELD_NOT_ALLOWED", "$.burst.arcMaxDegrees")
		end)
	end)

	harness:suite("chain lightning graph", function(h: any)
		h:test("the chain compiles to three reachable links", function()
			local graph = Presets.getGraph("ChainLightning")
			local compiled, diagnostics = Compiler.compileGraph(graph.root, graph.recipes)
			h:assertEqual(#diagnostics, 0)
			h:assertTrue(compiled ~= nil)
			h:assertTrue(compiled.recipes["chain_arc_two"] ~= nil)
			h:assertTrue(compiled.recipes["chain_arc_three"] ~= nil)
			h:assertTrue(compiled.recipes["chain_arc_four"] ~= nil)
		end)

		h:test("the last link has no burst, so the chain ends rather than being refused", function()
			-- A terminal link that still declared a burst would produce a
			-- ChildSpawnRejected on every single cast, which reads in the records
			-- as "your depth budget is wrong" forever.
			local graph = Presets.getGraph("ChainLightning")
			h:assertNil(graph.recipes["chain_arc_four"].burst)
			h:assertTrue(graph.recipes["chain_arc_three"].burst ~= nil)
		end)

		h:test("a depth budget below the chain length is refused at compile time", function()
			-- The point of the assertion: lowering maxBurstDepth does not quietly
			-- give a shorter chain, it gives a diagnostic. Anyone tuning the
			-- number should find that out here.
			local graph = Presets.getGraph("ChainLightning")
			local root = TableUtil.deepCopy(graph.root)
			root.budgets.maxBurstDepth = 1
			local compiled, diagnostics = Compiler.compileGraph(root, graph.recipes)
			h:assertNil(compiled)
			h:assertDiagnostic(
				diagnostics,
				"RECIPE_DEPTH_EXCEEDED",
				'$.recipes["chain_arc_two"].burst.children[1].recipeId'
			)
		end)

		h:test("a missing link is named rather than silently ending the chain", function()
			local graph = Presets.getGraph("ChainLightning")
			local recipes = TableUtil.deepCopy(graph.recipes)
			recipes["chain_arc_three"] = nil
			local compiled, diagnostics = Compiler.compileGraph(graph.root, recipes)
			h:assertNil(compiled)
			h:assertDiagnostic(
				diagnostics,
				"MISSING_RECIPE",
				'$.recipes["chain_arc_two"].burst.children[1].recipeId'
			)
		end)

		h:test("a link that referred to itself would be caught as a cycle", function()
			-- Why the chain is four documents and not one: the obvious way to
			-- write a chain is a recipe whose burst names itself, and the
			-- compiler refuses it. Asserted so the four-file shape is not
			-- mistaken for an accident.
			local graph = Presets.getGraph("ChainLightning")
			local recipes = TableUtil.deepCopy(graph.recipes)
			recipes["chain_arc_two"].burst.children[1].recipeId = "chain_arc_two"
			local compiled, diagnostics = Compiler.compileGraph(graph.root, recipes)
			h:assertNil(compiled)
			h:assertDiagnostic(
				diagnostics,
				"RECIPE_CYCLE",
				'$.recipes["chain_arc_two"].burst.children[1].recipeId'
			)
		end)
	end)

	harness:suite("arc launch directions", function(h: any)
		-- The directions come out of `SpellSolver`, which needs a whole cast to
		-- run. Rather than stand one up, this rebuilds the draw the solver makes
		-- from the same primitives -- one azimuth then one polar cosine, in that
		-- order -- and asserts the band. If the solver's draw order ever changes
		-- this test stops describing it, which is why it says so out loud.
		local RandomStreams = core.RandomStreams

		local function arcDirection(seed: number, minimum: number, maximum: number): any
			local random = RandomStreams.new(seed, "direction")
			local azimuth = random:NextNumber(0, math.pi * 2)
			local z = random:NextNumber(math.cos(math.rad(maximum)), math.cos(math.rad(minimum)))
			local radius = math.sqrt(math.max(0, 1 - z * z))
			return { x = radius * math.cos(azimuth), y = radius * math.sin(azimuth), z = z }
		end

		h:test("every drawn direction lands inside the authored band", function()
			local forward = { x = 0, y = 0, z = 1 }
			local low = math.cos(math.rad(68))
			local high = math.cos(math.rad(22))
			for seed = 1, 400 do
				local direction = arcDirection(seed * 7919, 22, 68)
				local cosine = dot(direction, forward)
				h:assertTrue(cosine >= low - 1e-9, "hop turned less than the floor")
				h:assertTrue(cosine <= high + 1e-9, "hop turned more than the ceiling")
			end
		end)

		h:test("no drawn direction is straight ahead, which a cone would allow", function()
			-- The whole reason Arc exists rather than a narrow Cone: a cone is
			-- solid, so it concentrates children near its axis and a chain built
			-- on one is a straight line with a wobble.
			local forward = { x = 0, y = 0, z = 1 }
			local nearest = -1
			for seed = 1, 400 do
				nearest = math.max(nearest, dot(arcDirection(seed * 7919, 22, 68), forward))
			end
			h:assertTrue(nearest < math.cos(math.rad(21)), "some hop was effectively straight on")
		end)

		h:test("directions stay unit length", function()
			for seed = 1, 64 do
				local direction = arcDirection(seed * 104729, 22, 68)
				h:assertNear(math.sqrt(dot(direction, direction)), 1, 1e-9)
			end
		end)
	end)

	harness:suite("splinterstorm graph", function(h: any)
		h:test("the shell compiles with its shard recipe", function()
			local graph = Presets.getGraph("Splinterstorm")
			local compiled, diagnostics = Compiler.compileGraph(graph.root, graph.recipes)
			h:assertEqual(#diagnostics, 0)
			h:assertTrue(compiled ~= nil)
			h:assertTrue(compiled.recipes["splinter_shard"] ~= nil)
		end)

		h:test("the shell throws eight shards along its travel", function()
			local document = Presets.get("Splinterstorm")
			h:assertEqual(document.burst.distribution, Enums.BurstDistribution.Cone)
			h:assertEqual(document.burst.children[1].count, 8)
			h:assertEqual(document.burst.children[1].recipeId, "splinter_shard")
		end)

		h:test("the shell and the shard agree on the laceration definition", function()
			-- compileGraph refuses two definitions that share an id and version
			-- without being identical, so this is the assertion that the two
			-- documents keep coming from one factory.
			local graph = Presets.getGraph("Splinterstorm")
			local compiled, diagnostics = Compiler.compileGraph(graph.root, graph.recipes)
			h:assertEqual(#diagnostics, 0)
			h:assertTrue(compiled ~= nil)
		end)

		h:test("a drifted duplicate definition is caught rather than merged", function()
			local graph = Presets.getGraph("Splinterstorm")
			local recipes = TableUtil.deepCopy(graph.recipes)
			recipes["splinter_shard"].statusDefinitions[1].duration = 99
			local compiled, diagnostics = Compiler.compileGraph(graph.root, recipes)
			h:assertNil(compiled)
			h:assertTrue(#diagnostics > 0)
		end)
	end)

	harness:suite("new presets are complete documents", function(h: any)
		for _, name in { "ChainLightning", "GlacialLance", "Splinterstorm" } do
			h:test(name .. " validates as authored", function()
				local valid, diagnostics = Validator.validate(Presets.get(name))
				if not valid then
					for _, diagnostic in diagnostics do
						print(name, diagnostic.code, diagnostic.path, diagnostic.message)
					end
				end
				h:assertTrue(valid, name .. " did not validate")
			end)

			h:test(name .. " round-trips through Builder.from unchanged", function()
				local document = Presets.get(name)
				h:assertDeepEqual(Builder.from(TableUtil.deepCopy(document)), document)
			end)
		end
	end)
end
