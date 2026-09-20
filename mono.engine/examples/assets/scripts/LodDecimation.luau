--!strict
-- Automatic mesh LOD with one built-in mesh and one EditableMesh source.
--
-- Both parts leave their generated levels unnamed. The renderer must build the
-- decimated artifacts from each base mesh, publish their triangle counts, and
-- select them when the observer camera becomes active.
--
-- ## Run it
--
--     client --script LodDecimation.luau
--
-- The view alternates every four seconds of simulated time. The close camera establishes the
-- detailed control; the observer camera starts at z=110 and exercises a
-- generated coarse level. The console identifies the active view so captures
-- can be paired with the selected distance.

local RunService = game:GetService("RunService")

local GRID_CELLS = 24
local GRID_SPACING = 0.5
local GRID_HALF = GRID_CELLS * GRID_SPACING * 0.5
local OBSERVER_PERIOD_SECONDS = 4

local function configureDecimation(part: MeshPart)
	part.AutoLodStrategy = Enum.AutoMeshLODStrategy.Decimated
	part.AutoLodLevels = 3
	part.AutoLod1Ratio = 0.35
	part.AutoLod2Ratio = 0.12
	-- At the observer view each source projects to only a few pixels per base
	-- triangle. Eight pixels per triangle preserves the close control and makes
	-- the observer select a generated coarse level.
	part.AutoLodTargetQuadArea = 8
end

local floor = Instance.new("Part")
floor.Name = "LodDecimationFloor"
floor.Anchored = true
floor.Size = Vector3.new(34, 1, 28)
floor.Position = Vector3.new(0, -0.5, 0)
floor.Color = Color3.fromRGB(20, 24, 32)
floor.Parent = workspace

-- A normal mesh source. `engine.Sphere` is generated in process, so the scene
-- can prove automatic LOD without depending on a downloaded content store.
local normal = Instance.new("MeshPart")
normal.Name = "NormalMeshLodSource"
normal.Anchored = true
normal.MeshId = "engine.Sphere"
normal.Size = Vector3.new(9, 9, 9)
normal.Position = Vector3.new(-9, 4.5, 0)
normal.Color = Color3.fromRGB(72, 164, 255)
configureDecimation(normal)
normal.Parent = workspace

-- A shared-vertex curved grid leaves many valid edge collapses for the
-- decimator. A uniform part colour keeps every triangle in one material run,
-- so material seams cannot disguise a failed reduction.
local editable = Instance.new("EditableMesh")
local vertices: { number } = {}

for row = 0, GRID_CELLS do
	for column = 0, GRID_CELLS do
		local x = column * GRID_SPACING - GRID_HALF
		local z = row * GRID_SPACING - GRID_HALF
		local y = math.sin(x * 0.8) * math.cos(z * 0.8) * 1.3
		local vertex = editable:AddVertex(Vector3.new(x, y, z))
		assert(vertex ~= nil, "EditableMesh refused a grid vertex")
		vertices[row * (GRID_CELLS + 1) + column + 1] = vertex :: number
	end
end

for row = 0, GRID_CELLS - 1 do
	for column = 0, GRID_CELLS - 1 do
		local topLeft = vertices[row * (GRID_CELLS + 1) + column + 1]
		local topRight = vertices[row * (GRID_CELLS + 1) + column + 2]
		local bottomLeft = vertices[(row + 1) * (GRID_CELLS + 1) + column + 1]
		local bottomRight = vertices[(row + 1) * (GRID_CELLS + 1) + column + 2]
		editable:AddTriangle(topLeft, bottomLeft, topRight)
		editable:AddTriangle(topRight, bottomLeft, bottomRight)
	end
end

assert(editable.TriangleCount == GRID_CELLS * GRID_CELLS * 2, "editable LOD source is incomplete")

local editablePart = Instance.new("MeshPart")
editablePart.Name = "EditableMeshLodSource"
editablePart.Anchored = true
editablePart.MeshId = editable.ContentId
-- `Size` fits the authored editable bounds into this visible volume. Keeping
-- the grid at its authored twelve metre width makes it readable in observer
-- captures and keeps the chosen projected-area budget meaningful.
editablePart.Size = Vector3.new(GRID_CELLS * GRID_SPACING, 3, GRID_CELLS * GRID_SPACING)
editablePart.Position = Vector3.new(9, 3, 0)
editablePart.Color = Color3.fromRGB(255, 154, 74)
configureDecimation(editablePart)
editablePart.Parent = workspace

local mainCamera = Instance.new("Camera")
mainCamera.Name = "LodMainCamera"
mainCamera.FieldOfView = 48
mainCamera.FarPlaneZ = 180
mainCamera.CFrame = CFrame.lookAt(Vector3.new(0, 24, 31), Vector3.new(0, 3.5, 0))
mainCamera.Parent = workspace

local observerCamera = Instance.new("Camera")
observerCamera.Name = "LodObserverCamera"
observerCamera.FieldOfView = 70
observerCamera.FarPlaneZ = 180
observerCamera.CFrame = CFrame.lookAt(Vector3.new(0, 70, 110), Vector3.new(0, 3.5, 0))
observerCamera.Parent = workspace

workspace.CurrentCamera = mainCamera

local observing = false
local simulatedSeconds = 0
RunService.Heartbeat:Connect(function(deltaTime: number)
	simulatedSeconds += deltaTime
	local shouldObserve = math.floor(simulatedSeconds / OBSERVER_PERIOD_SECONDS) % 2 == 1
	if shouldObserve == observing then
		return
	end

	observing = shouldObserve
	workspace.CurrentCamera = if observing then observerCamera else mainCamera
	print(if observing then "lod decimation: observer camera" else "lod decimation: main camera")
end)
