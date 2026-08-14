--!strict
-- A checkered floor running to the horizon, which is the one thing a mip chain
-- is visible in.
--
-- **The renderer has no unit suite and no mock, by rule**, so "are the levels
-- uploaded and sampled" was answered by compiling and by the format tests and
-- nothing else. Three modules can each look correct and the chain still never
-- reach a fragment: `max_lod` sat at zero for four versions while
-- `mipmap_mode` said LINEAR, which clamps every fetch to level zero and is
-- invisible in every test that does not involve a GPU and a minified texture.
--
-- ## What makes this measurable rather than a screenshot
--
-- A high-frequency texture minified past one texel per pixel aliases. Without a
-- chain the distance is speckle - neighbouring pixels land on unrelated texels,
-- so the region has **high local variance**. With a chain the sampler fetches a
-- level whose texels are already the average, and the same region goes smooth:
-- **low variance**, tending to the sheet's mean.
--
-- So the probe is not "does it look right". It is: sample the far strip, take
-- the mean absolute difference between neighbouring pixels, and compare the two
-- builds. A working chain drops that number hard. A frame that merely *changed*
-- proves nothing - the number has to fall.
--
-- **The floor is one long part rather than a row of them**, because a seam
-- between parts is its own edge and would contribute variance the texture did
-- not.
--
-- ## Run it
--
--     studio --headless --frames 200 --run server --capture-world <this> --capture out.bmp
--
-- Headless capture pins the animation clock, so two runs of one build agree to
-- about a fifth of a percent - see `Options::FixedAnimationStep`. Anything
-- larger than that is the change under test.

-- **A grid of small tiles rather than one long floor, and the first version of
-- this got it wrong.** A `MeshPart` maps its texture 0..1 across a face, so one
-- part stretched nine hundred units wears a single checker - nothing tiles,
-- nothing is minified, and the probe measured a number near zero for both
-- builds because there was no high frequency to lose. Each tile here wears the
-- whole sheet, so a tile a few pixels wide at the far end is the sheet squeezed
-- into those pixels, which is exactly the case a chain exists for.
--
-- The seams between tiles are their own edges and contribute variance the
-- texture did not. That is acceptable because they contribute it **equally to
-- both builds**, so the comparison survives what the absolute number does not.
local TILE = 4
local TILES_ACROSS = 9
local TILES_DEEP = 90

for row = 1, TILES_DEEP do
	for column = 1, TILES_ACROSS do
		local tile = Instance.new("MeshPart")
		tile.Name = `Tile{row}_{column}`
		tile.Anchored = true
		tile.MeshId = "engine.Cube"

		-- **The built-in checker, and it is the point.** Generated in process by
		-- `assets::MakeBuiltin` rather than delivered, so this probe needs no CDN
		-- and no published content.
		tile.TextureID = "engine.Checker"
		tile.Size = Vector3.new(TILE, 0.5, TILE)
		tile.CFrame = CFrame.new(
			Vector3.new((column - (TILES_ACROSS + 1) / 2) * TILE, 0, -row * TILE)
		)
		tile.Parent = workspace
	end
end

-- Looking down the grid from above it. High enough that the floor fills the
-- frame rather than sitting in a band, shallow enough that the far tiles are a
-- few pixels each - which is where a sampler clamped to level zero has nothing
-- to average and picks one texel of many.
local view = Instance.new("Camera")
view.Name = "GrazingCamera"
view.CFrame = CFrame.lookAt(Vector3.new(0, 9, 10), Vector3.new(0, 0, -TILES_DEEP * TILE))
view.Parent = workspace
workspace.CurrentCamera = view

-- **One marker near the camera, and it is the control.** If a change smoothed
-- the whole frame - a resolution change, a blur, a texture that stopped
-- resolving - this would smooth too. A mip chain must leave it alone.
local post = Instance.new("Part")
post.Name = "Marker"
post.Anchored = true
post.Size = Vector3.new(2, 5, 2)
post.CFrame = CFrame.new(Vector3.new(-13, 2.5, -14))
post.Color = Color3.new(0.85, 0.25, 0.30)
post.Parent = workspace

print("mipprobe: tiled checker grid, camera looking down it")
