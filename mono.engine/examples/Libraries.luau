--!strict
-- The shipped Luau libraries, loaded and exercised.
--
-- `MagicCore` and `TerrainCore` came from a Rojo project where they sat under
-- `ReplicatedStorage`, and they are here **unchanged** - not one line of either
-- library was edited to make it run in this engine. What makes that possible is
-- `script::MountModuleTree`: the staged `assets/lib/` directory is mirrored into
-- `ModuleScript` instances under `ReplicatedStorage` before any scene runs, so
-- `require(script.Parent.Compiler)` inside the library resolves exactly the way
-- it did in Studio.
--
-- This scene is the proof of that and nothing else. `Magic.luau` is the one that
-- builds a world out of them.
--
-- ## What a port had to survive
--
-- Very little, which is the interesting part. Between them the two libraries
-- reach outside pure Luau in three places:
--
--   * `Random.new(seed)` - this engine has it, and its sequence is a pure
--     function of the seed and the draw index, which is exactly what
--     `RandomStreams` needs. The *numbers* differ from Roblox's, so anything
--     asserting a literal draw would disagree; nothing does.
--   * `typeof` - a Luau fastcall builtin, so it is there whether or not a host
--     installs a global.
--   * `bit32` and `table.freeze` - Luau, not Roblox.
--
-- `MagicCore.Vector3` is the library's own module and shadows nothing, and
-- neither library ever touches an Instance. That is what "pure data core" bought.

local ReplicatedStorage = game:GetService("ReplicatedStorage")

local MagicCore = ReplicatedStorage:FindFirstChild("MagicCore")
assert(MagicCore ~= nil, "ReplicatedStorage.MagicCore was not mounted - is assets/lib staged?")

local TerrainCore = ReplicatedStorage:FindFirstChild("TerrainCore")
assert(TerrainCore ~= nil, "ReplicatedStorage.TerrainCore was not mounted")

-- --- magic -----------------------------------------------------------------

local Presets = require(MagicCore:FindFirstChild("Presets") :: any)
local Compiler = require(MagicCore:FindFirstChild("Compiler") :: any)
local Validator = require(MagicCore:FindFirstChild("Validator") :: any)

local compiled = 0
local failed = 0

for _, name in Presets.names() do
	local graph = Presets.getGraph(name)

	local valid, diagnostics = Validator.validate(graph.root)
	if not valid then
		failed += 1
		warn(`{name}: {#diagnostics} validation diagnostic(s), first at {diagnostics[1].path}`)
		continue
	end

	local program, graphDiagnostics = Compiler.compileGraph(graph.root, graph.recipes)
	if program == nil then
		failed += 1
		warn(`{name}: compile failed at {graphDiagnostics[1].path} - {graphDiagnostics[1].message}`)
		continue
	end

	compiled += 1
end

print(`libraries: {compiled} preset(s) compiled, {failed} failed`)

-- --- terrain ---------------------------------------------------------------

local Terrain = require(TerrainCore :: any)

local edits = Terrain.Edits.new()
local field = Terrain.HeightField.new({ seed = 20260807 }, edits)

-- A coarse sweep, so the log says whether the height field is doing anything at
-- all. A generator whose whole map is one flat plane is the failure that looks
-- like success, and it is invisible from a single sample.
local lowest = math.huge
local highest = -math.huge
local wet = 0
local samples = 0

for z = -4000, 4000, 401 do
	for x = -4000, 4000, 401 do
		local height = field:heightAt(x, z)
		lowest = math.min(lowest, height)
		highest = math.max(highest, height)
		if height < field.settings.seaLevel then
			wet += 1
		end
		samples += 1
	end
end

print(
	`terrain: height {string.format("%.1f", lowest)} to {string.format("%.1f", highest)}, `
		.. `{string.format("%.0f", wet / samples * 100)}% below sea level`
)

-- One chunk, meshed, and then the same chunk with a hole blown in it.
local before = Terrain.Chunker.build(field, 0, 0, 32)
edits:crater(16, 16, 13, 9, 1)
local after = Terrain.Chunker.build(field, 0, 0, 32)

print(`terrain: chunk (0, 0) is {before.count} boxes, {after.count} after one crater`)

-- --- something to look at ---------------------------------------------------
--
-- The scene has to build *something*, or `LoadScene` measures a world one unit
-- across and the camera frames nothing. One row of boxes off the meshed chunk is
-- enough to prove the data reached the world.

local SURFACES = Terrain.HeightField.Surfaces

local shown = 0
for _, box in after.boxes do
	if shown >= 240 then
		break
	end
	shown += 1

	local width = box.x1 - box.x0 + 1
	local height = box.top - box.bottom + 1
	local depth = box.z1 - box.z0 + 1

	local part = Instance.new("Part")
	part.Name = "Voxels"
	part.Anchored = true
	part.Size = Vector3.new(width, height, depth)
	part.Position = Vector3.new(
		box.x0 + width * 0.5,
		box.bottom + height * 0.5,
		box.z0 + depth * 0.5
	)

	local surface = SURFACES[box.surface]
	part.Color = Color3.new(surface.color.r, surface.color.g, surface.color.b)
	part.Parent = workspace
end

local eye = Instance.new("Camera")
eye.Name = "Eye"
eye.CFrame = CFrame.lookAt(Vector3.new(-30, 80, -30), Vector3.new(16, 40, 16))
eye.FieldOfView = 70
eye.Parent = workspace
workspace.CurrentCamera = eye

print(`libraries: {shown} boxes drawn`)
