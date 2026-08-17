--!strict
-- One bay per kind of asset, so a single frame says which kinds work.
--
-- `MeshGrid.luau` answers one question thoroughly - did every *mesh* arrive and
-- draw. This answers a wider and shallower one: of the six kinds this engine
-- can name, which reach the screen at all. A bay that shows its fallback is a
-- pipeline that broke, and which bay it is says where.
--
-- ## Why bays rather than a grid
--
-- A grid is right when every cell is the same kind of thing and the question is
-- "which of them". Here every cell is a *different* path - a mesh goes through
-- `MeshTable`, a texture through `TextureTable`, an image through
-- `InterfacePass`, a sound through the mixer - so the useful layout is one
-- labelled bay each, in a fixed order, with the label rendered in world rather
-- than written down here. A frame nobody has to hold a key for is the point.
--
-- ## What each bay proves, and what its failure looks like
--
--   * **MeshPart** - geometry resolved by name. Failure draws `MeshTable`'s
--     fallback cube, so a cube in the mesh bay is a name that did not match.
--   * **Texture** - a still sheet sampled in the 3D pass. Failure wears the
--     purple checkerboard `MissingTexture` hands back.
--   * **GIF** - the same path with `TextureData`'s flipbook triple set, so the
--     sheet is a grid of frames and the shader walks it. Failure is a bay that
--     shows one frame and never moves, which is why the bay beside it is a
--     still on purpose: the pair is the comparison.
--   * **Image** - the same bytes through the *interface* pass instead, on a
--     `SurfaceGui`. Two bays wearing the same texture and only one of them
--     right says the fault is in a pass rather than in content.
--   * **Audio** - nothing to look at, so the marker above it moves while the
--     sound is playing. A still marker is a sound that did not load.
--   * **Video** - a placeholder that says so. `AssetKind::Video` is a routing
--     label today: `mp4` maps to it and nothing decodes it, so a bay pretending
--     otherwise would be the one lie in the scene.
--
-- ## Run it
--
--     assetc --input ART --output content --no-copy --model-size 1
--     cdn --publish content --store store --signing-key HEX
--     client --cdn dir:store --publisher-key PUBLIC --script Assets.luau
--
-- With no `--cdn` every content bay falls back to a built-in and says so in the
-- log, which is a scene worth looking at rather than an empty floor.

local RunService = game:GetService("RunService")

-- Bays are two and a half metres apart on X, which fits six across a 70 degree
-- frame from the camera below without the outer two going off the edge.
local BAY_SPACING = 9
local BAY_COUNT = 6
local PEDESTAL_HEIGHT = 1.5
local SHOWPIECE_SIZE = 4

-- Where bay `index` (1-based) stands, centred about the origin.
local function bayX(index: number): number
	return (index - (BAY_COUNT + 1) / 2) * BAY_SPACING
end

-- The floor. Wide enough that the outer bays are not standing on the edge,
-- which reads as the scene being cropped.
local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(BAY_COUNT * BAY_SPACING + 12, 1, 24)
floor.CFrame = CFrame.new(Vector3.new(0, -0.5, 0))
floor.Color = Color3.new(0.22, 0.22, 0.24)
floor.Parent = workspace

-- A pedestal and a floating label, which is every bay's furniture.
--
-- **The label is a `BillboardGui` and not a `SurfaceGui`**, because a bay is
-- read from wherever the camera happens to be and a surface label is only
-- legible from in front of the face it is on. This is the one piece of the
-- scene whose job is to be readable rather than to be tested.
local function makeBay(index: number, title: string): Part
	local pedestal = Instance.new("Part")
	pedestal.Name = title .. "Pedestal"
	pedestal.Anchored = true
	pedestal.Size = Vector3.new(5, PEDESTAL_HEIGHT, 5)
	pedestal.CFrame = CFrame.new(Vector3.new(bayX(index), PEDESTAL_HEIGHT / 2, 0))
	pedestal.Color = Color3.new(0.32, 0.32, 0.36)
	pedestal.Parent = workspace

	local billboard = Instance.new("BillboardGui")
	billboard.Name = title .. "Label"
	billboard.Adornee = pedestal
	billboard.Size = UDim2.new(0, 200, 0, 44)
	billboard.StudsOffset = Vector3.new(0, 4.5, 0)

	-- Over the showpiece rather than behind it. A label a mesh occludes is a
	-- label that disappears exactly when the bay is working.
	billboard.AlwaysOnTop = true
	billboard.Parent = pedestal

	local text = Instance.new("TextLabel")
	text.Name = "Title"
	text.Size = UDim2.new(1, 0, 1, 0)
	text.BackgroundTransparency = 1
	text.Text = title
	text.TextColor3 = Color3.new(1, 1, 1)
	text.TextScaled = true
	text.Parent = billboard

	return pedestal
end

-- Where the showpiece in a bay sits: clear of its pedestal, centred on it.
local function showpieceFrame(index: number): CFrame
	return CFrame.new(Vector3.new(bayX(index), PEDESTAL_HEIGHT + SHOWPIECE_SIZE / 2, 0))
end

-- --- the six bays ------------------------------------------------------------

makeBay(1, "MeshPart")
makeBay(2, "Texture")
makeBay(3, "GIF")
makeBay(4, "Image")
makeBay(5, "Audio")
makeBay(6, "Video")

-- **Bay 1 - geometry.** Starts on a built-in so the bay is never empty, and is
-- re-pointed at the first published mesh when one arrives. `engine.Cube` is
-- generated in process by `assets::MakeBuiltin` and cannot fail to resolve, so
-- a wrong-looking bay here before content lands is a renderer fault.
local meshShow = Instance.new("MeshPart")
meshShow.Name = "MeshShowpiece"
meshShow.Anchored = true
meshShow.MeshId = "engine.Cube"
meshShow.Size = Vector3.new(SHOWPIECE_SIZE, SHOWPIECE_SIZE, SHOWPIECE_SIZE)
meshShow.CFrame = showpieceFrame(1)
meshShow.Parent = workspace

-- **Bay 2 - a still sheet in the 3D pass**, and bay 3 the animated one. Both
-- start on the built-in checker so the difference between them is visible
-- before anything is published: same mesh, same size, one texture each.
local textureShow = Instance.new("MeshPart")
textureShow.Name = "TextureShowpiece"
textureShow.Anchored = true
textureShow.MeshId = "engine.Cube"
textureShow.TextureID = "engine.Checker"
textureShow.Size = Vector3.new(SHOWPIECE_SIZE, SHOWPIECE_SIZE, SHOWPIECE_SIZE)
textureShow.CFrame = showpieceFrame(2)
textureShow.Parent = workspace

local gifShow = Instance.new("MeshPart")
gifShow.Name = "GifShowpiece"
gifShow.Anchored = true
gifShow.MeshId = "engine.Cube"
gifShow.TextureID = "engine.Checker"
gifShow.Size = Vector3.new(SHOWPIECE_SIZE, SHOWPIECE_SIZE, SHOWPIECE_SIZE)
gifShow.CFrame = showpieceFrame(3)
gifShow.Parent = workspace

-- **Bay 4 - the same bytes through the interface pass.** A flat pane carrying a
-- `SurfaceGui`, so the texture is sampled by `interface.frag` rather than by
-- `opaque.frag`. Two bays wearing one texture through two passes is what makes
-- a broken pass distinguishable from broken content.
local imagePane = Instance.new("Part")
imagePane.Name = "ImagePane"
imagePane.Anchored = true
imagePane.Size = Vector3.new(SHOWPIECE_SIZE, SHOWPIECE_SIZE, 0.25)
imagePane.CFrame = showpieceFrame(4)
imagePane.Color = Color3.new(0.1, 0.1, 0.12)
imagePane.Parent = workspace

local imageSurface = Instance.new("SurfaceGui")
imageSurface.Name = "ImageSurface"
imageSurface.Adornee = imagePane

-- The camera below stands on +Z, which looks at the part's Back face.
imageSurface.Face = Enum.NormalId.Back
imageSurface.Parent = imagePane

local imageLabel = Instance.new("ImageLabel")
imageLabel.Name = "Showpiece"
imageLabel.Size = UDim2.new(1, 0, 1, 0)
imageLabel.BackgroundTransparency = 1
imageLabel.Image = "engine.Checker"
imageLabel.Parent = imageSurface

-- **Bay 5 - audio, which has nothing to draw.** The marker is the readout: it
-- rises and falls while the sound is playing, so a bay that never moves is a
-- sound that did not load. Cheaper and more honest than a fake waveform, which
-- would animate whether or not anything was audible.
local audioMarker = Instance.new("Part")
audioMarker.Name = "AudioMarker"
audioMarker.Anchored = true
audioMarker.Size = Vector3.new(1.5, 1.5, 1.5)
audioMarker.CFrame = showpieceFrame(5)
audioMarker.Color = Color3.new(0.30, 0.75, 0.45)
audioMarker.Parent = workspace

local sound = Instance.new("Sound")
sound.Name = "Showpiece"
sound.Volume = 0.4
sound.Parent = audioMarker

-- **Bay 6 - the honest one.** `AssetKind::Video` exists as a routing label and
-- `mp4` maps to it; there is no decoder, no player and no runtime path. A bay
-- showing a stand-in picture would be the only thing in this scene claiming
-- something works when it does not, so it shows its own absence instead.
local videoPane = Instance.new("Part")
videoPane.Name = "VideoPane"
videoPane.Anchored = true
videoPane.Size = Vector3.new(SHOWPIECE_SIZE, SHOWPIECE_SIZE * 0.5625, 0.25)
videoPane.CFrame = showpieceFrame(6)
videoPane.Color = Color3.new(0.12, 0.12, 0.14)
videoPane.Parent = workspace

local videoSurface = Instance.new("SurfaceGui")
videoSurface.Name = "VideoSurface"
videoSurface.Adornee = videoPane
videoSurface.Face = Enum.NormalId.Back
videoSurface.Parent = videoPane

local videoText = Instance.new("TextLabel")
videoText.Name = "Absent"
videoText.Size = UDim2.new(1, 0, 1, 0)
videoText.BackgroundTransparency = 1
videoText.Text = "no decoder"
videoText.TextColor3 = Color3.new(0.75, 0.55, 0.30)
videoText.TextScaled = true
videoText.Parent = videoSurface

-- --- the camera ---------------------------------------------------------------

local view = Instance.new("Camera")
view.Name = "AssetsCamera"

-- Back far enough for six bays and high enough to see the floor they stand on,
-- looking at the middle of the row rather than at the origin - the row is
-- centred on X but its interesting half is above the pedestals.
view.CFrame = CFrame.lookAt(Vector3.new(0, 9, 34), Vector3.new(0, 4, 0))
view.Parent = workspace

-- **Parenting a camera is not aiming with it.** `ActiveCamera` names the live
-- one, so a camera that is only parented is a row nothing reads.
workspace.CurrentCamera = view

-- --- content, as it arrives ----------------------------------------------------

-- Content is fetched over frames, so nothing published is in this world when
-- the chunk above runs. Polling is the honest shape: a callback firing mid-tick
-- is what rule 5 forbids.
local placedMesh = false
local placedStill = false
local placedFlipbook = false
local reported = false

-- **Counted rather than read off a clock.** `tick()` is the wall, so the same
-- frame of two runs holds a different pose - which makes this scene impossible
-- to compare against a capture of itself, and comparing captures is how a
-- rendering change gets checked at all. A frame counter costs one add and makes
-- frame four hundred mean one thing.
local beats = 0

RunService.Heartbeat:Connect(function()
	beats += 1
	local elapsed = beats / 60

	-- The mesh bay turns, because a still frame hides the half of a mesh facing
	-- away - and a reversed winding is exactly the bug that looks fine from the
	-- front.
	meshShow.CFrame = showpieceFrame(1) * CFrame.Angles(0, elapsed * 0.6, 0)

	-- The two texture bays turn together at the same rate, so the only
	-- difference the eye has to catch between them is whether the sheet moves.
	textureShow.CFrame = showpieceFrame(2) * CFrame.Angles(0, elapsed * 0.4, 0)
	gifShow.CFrame = showpieceFrame(3) * CFrame.Angles(0, elapsed * 0.4, 0)

	-- The audio marker's readout. Bobbing rather than scaling, because a part
	-- that changes size also changes what the physics broadphase holds and this
	-- scene should not be poking that to draw a meter.
	audioMarker.CFrame =
		showpieceFrame(5) * CFrame.new(Vector3.new(0, math.sin(elapsed * 3) * 0.6, 0))

	if not placedMesh then
		local meshes = ContentService:GetMeshes()
		if #meshes > 0 then
			meshShow.MeshId = meshes[1]
			placedMesh = true
			print(`assets: mesh bay showing {meshes[1]} - {ContentService:GetTriangleCount(meshes[1])} triangle(s)`)
		end
	end

	if not (placedStill and placedFlipbook) then
		-- **Sorted into the two bays by what the texture actually is**, which
		-- `GetFlipbook` is the only thing that can answer: a baked GIF and a
		-- baked PNG are both one `.atex`, and the difference is the triple
		-- recorded inside it rather than anything about the name.
		for _, texture in ipairs(ContentService:GetTextures()) do
			local flipbook = ContentService:GetFlipbook(texture)

			if flipbook ~= nil and not placedFlipbook then
				gifShow.TextureID = texture
				placedFlipbook = true
				print(
					`assets: gif bay showing {texture} - {flipbook.Frames} frame(s) at {flipbook.FrameRate} fps`
				)
			elseif flipbook == nil and not placedStill then
				textureShow.TextureID = texture
				imageLabel.Image = texture
				placedStill = true
				print(`assets: texture and image bays showing {texture}`)
			end
		end
	end

	-- Said once, and only after there has been a chance for content to land.
	if not reported and elapsed > 3 then
		reported = true
		if not (placedMesh or placedStill or placedFlipbook) then
			print("assets: nothing published in this world - run with --cdn to fill the content bays")
		end
		if not placedFlipbook then
			print("assets: no animated texture published - the gif bay is showing a still")
		end
	end
end)
