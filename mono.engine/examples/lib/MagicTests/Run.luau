--!strict

local Harness = require(script.Parent.Harness)
local Fixtures = require(script.Parent.Fixtures)

local dataSpecs = {
	require(script.Parent.Specs.BuilderCompiler),
	require(script.Parent.Specs.Validation),
	require(script.Parent.Specs.RandomEvent),
	require(script.Parent.Specs.ProjectileVisual),
	require(script.Parent.Specs.ActorAreaEffect),
	require(script.Parent.Specs.StatusEffects),
	require(script.Parent.Specs.SpellPreset),
	require(script.Parent.Specs.ArcChain),
	require(script.Parent.Specs.StatusContent),
	require(script.Parent.Specs.TerrainField),
}

local EXPECTED_DATA_TEST_COUNT = 215

local Run = {}

-- `FindFirstChild` rather than Roblox's `WaitForChild`, which this engine does
-- not have. Nothing here is streamed in: `MountModuleTree` builds the whole tree
-- before any script runs, so a missing child is a staging bug and waiting for it
-- would only turn a clear failure into a hang.
local function moduleChild(parent: Instance, name: string): ModuleScript
	local child = parent:FindFirstChild(name)
	assert(child ~= nil, parent:GetFullName() .. " has no child " .. name)
	assert(child:IsA("ModuleScript"), parent:GetFullName() .. "." .. name .. " must be a ModuleScript")
	return child
end

local function loadCore(coreRoot: Instance): any
	return {
		ActorState = require(moduleChild(coreRoot, "ActorState")),
		AreaSolver = require(moduleChild(coreRoot, "AreaSolver")),
		Builder = require(moduleChild(coreRoot, "Builder")),
		Compiler = require(moduleChild(coreRoot, "Compiler")),
		Components = require(moduleChild(coreRoot, "Components")),
		EffectSolver = require(moduleChild(coreRoot, "EffectSolver")),
		Enums = require(moduleChild(coreRoot, "Enums")),
		EventSolver = require(moduleChild(coreRoot, "EventSolver")),
		Presets = require(moduleChild(coreRoot, "Presets")),
		ProjectileSolver = require(moduleChild(coreRoot, "ProjectileSolver")),
		RandomStreams = require(moduleChild(coreRoot, "RandomStreams")),
		SpellSolver = require(moduleChild(coreRoot, "SpellSolver")),
		StatusDefinitions = require(moduleChild(coreRoot, "StatusDefinitions")),
		StatusEffectSolver = require(moduleChild(coreRoot, "StatusEffectSolver")),
		TableUtil = require(moduleChild(coreRoot, "TableUtil")),
		Validator = require(moduleChild(coreRoot, "Validator")),
		VisualLifecycle = require(moduleChild(coreRoot, "VisualLifecycle")),
	}
end

-- `TerrainCore` is optional: it is a separate library that happens to be tested
-- by the same harness, and a place that installed only MagicCore should still be
-- able to run the magic suite. `TerrainField` skips itself when this is nil.
local function loadTerrain(terrainRoot: Instance?): any
	if terrainRoot == nil then
		return nil
	end
	return {
		Noise = require(moduleChild(terrainRoot, "Noise")),
		NoiseMap = require(moduleChild(terrainRoot, "NoiseMap")),
		HeightField = require(moduleChild(terrainRoot, "HeightField")),
		Edits = require(moduleChild(terrainRoot, "Edits")),
		Chunker = require(moduleChild(terrainRoot, "Chunker")),
	}
end

function Run.data(coreRoot: Instance, terrainRoot: Instance?): any
	local harness = Harness.new("data-only")
	local core = loadCore(coreRoot)
	local terrain = loadTerrain(terrainRoot)
	for _, spec in ipairs(dataSpecs) do
		spec(harness, core, Fixtures, terrain)
	end
	harness:suite("harness contracts", function(h: any)
		h:test("assertNear rejects nonfinite values and invalid tolerances", function()
			h:assertThrows(function()
				h:assertNear(0 / 0, 0)
			end, "finite numbers")
			h:assertThrows(function()
				h:assertNear(math.huge, 0)
			end, "finite numbers")
			h:assertThrows(function()
				h:assertNear(0, -math.huge)
			end, "finite numbers")
			h:assertThrows(function()
				h:assertNear(0, 0, -1)
			end, "finite and non-negative")
			h:assertThrows(function()
				h:assertNear(0, 0, 0 / 0)
			end, "finite and non-negative")
			h:assertThrows(function()
				h:assertNear(0, 0, math.huge)
			end, "finite and non-negative")
		end)
	end)
	harness:suite("test inventory", function(h: any)
		h:test("declared data test count stays exact", function()
			h:assertEqual(harness:report().declared, EXPECTED_DATA_TEST_COUNT)
		end)
	end)
	return harness:report()
end


Run.Data = Run.data

return table.freeze(Run)
