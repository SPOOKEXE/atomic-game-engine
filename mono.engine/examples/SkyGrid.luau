--!strict
-- A skygrid: a lattice of blocks in an empty sky, and nothing underneath.
--
-- `Mirrors-1-world.luau` proves the rendering path - shadows, a surface camera,
-- a texture sampled back. This proves the **other** thing a viewport has to
-- survive: a world made of many small separate objects with sky visible between
-- every one of them, rather than a few large ones over a floor.
--
-- ## Why a grid rather than a pile
--
-- Depth sorting, culling and shadow fitting all behave differently when the
-- geometry is sparse. A baseplate hides the far plane and fills the frustum, so
-- a scene built on one never exercises the case where most of what is on screen
-- is background. Every block here is separated from its neighbours by empty
-- space in all three axes, so the frustum is mostly sky and the cull actually
-- has something to reject.
--
-- It is also the cheapest scene that is *obviously wrong* when something
-- breaks. A lattice has straight lines in three directions: a projection with
-- the wrong aspect bends them, a viewport set to the wrong rectangle shears
-- them, and a target sampled past its drawn region repeats the edge column.
-- None of those are visible on a baseplate.
--
-- ## What it is not
--
-- There is no terrain, no collision and nothing to stand on. The blocks are
-- anchored and stay where they are put - this is a thing to look at and to
-- measure a frame against, not a place to walk around in.

local RunService = game:GetService("RunService")

-- **Extent in blocks, not in studs.** The lattice is this many blocks along
-- each axis, so the count is the product and the size in studs follows from the
-- spacing below.
local COLUMNS = 7
local LAYERS = 4
local ROWS = 7

-- How far apart the blocks sit, centre to centre. Four times the block size,
-- which is what leaves sky visible between them from every angle rather than
-- only along the axes.
local SPACING = 9
local BLOCK = 2.25

-- Where the lattice hangs. Centred on the origin horizontally and lifted, so
-- the camera below looks up into it and sees the underside.
local ORIGIN = Vector3.new(
	-(COLUMNS - 1) * SPACING / 2,
	6,
	-(ROWS - 1) * SPACING / 2
)

-- **A fixed palette walked in order rather than a random colour per block.** A
-- random scene is a different scene every run, and a frame time compared
-- against a different scene is not a comparison. Walking a list keeps the same
-- lattice every time while still giving neighbours different colours, which is
-- what makes the depth order readable by eye.
local PALETTE = {
	Color3.fromRGB(196, 84, 72),
	Color3.fromRGB(214, 158, 74),
	Color3.fromRGB(122, 176, 96),
	Color3.fromRGB(78, 150, 196),
	Color3.fromRGB(140, 108, 190),
	Color3.fromRGB(206, 206, 206),
}

local blocks = {}
local index = 0

for column = 0, COLUMNS - 1 do
	for layer = 0, LAYERS - 1 do
		for row = 0, ROWS - 1 do
			index += 1

			local block = Instance.new("Part")
			block.Name = `Block_{column}_{layer}_{row}`
			block.Anchored = true
			block.Size = Vector3.new(BLOCK, BLOCK, BLOCK)
			block.Position = ORIGIN
				+ Vector3.new(column * SPACING, layer * SPACING, row * SPACING)
			block.Color = PALETTE[(index - 1) % #PALETTE + 1]
			block.Parent = workspace

			-- The phase is per block and derived from where it is, so the drift
			-- below travels across the lattice as a wave instead of moving every
			-- block together. Together would read as the camera moving.
			table.insert(blocks, {
				Part = block,
				Base = block.Position,
				Phase = (column + layer + row) * 0.6,
			})
		end
	end
end

-- The camera the scene is watched through. Below and outside the lattice,
-- looking up its diagonal - the angle where the three axes are all foreshortened
-- differently, so a projection error shows up as a bend rather than hiding
-- along one of them.
local view = Instance.new("Camera")
view.Name = "Viewer"
view.CFrame = CFrame.lookAt(
	Vector3.new(-26, -8, -30),
	Vector3.new(0, 6 + (LAYERS - 1) * SPACING / 2, 0)
)
view.FieldOfView = 70
view.Parent = workspace
workspace.CurrentCamera = view

-- **A slow drift rather than a spin.** The point of the motion is that the
-- frame is not identical twice in a row - a still scene lets a driver reuse
-- everything and reports a frame time nobody will ever see. Small enough that
-- the lattice still reads as a lattice.
local DRIFT = 0.55
local SPEED = 0.9

RunService.Heartbeat:Connect(function(deltaTime: number)
	for _, block in blocks do
		block.Phase += SPEED * deltaTime
		block.Part.Position = block.Base
			+ Vector3.new(0, math.sin(block.Phase) * DRIFT, 0)
	end
end)

print(`skygrid: {#blocks} blocks, {COLUMNS}x{LAYERS}x{ROWS} at {SPACING} spacing`)
