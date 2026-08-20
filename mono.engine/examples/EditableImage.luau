--!strict
-- A target painted with `DrawCircle` and `DrawLine`, applied to a part as its
-- texture - and repainted every frame, to prove the drawing half is live in
-- the same way `EditableMesh.luau` proves the geometry half is.
--
-- ## Run it
--
--     client --script EditableImage.luau

local RunService = game:GetService("RunService")

local image = Instance.new("EditableImage")
image:Resize(128, 128)

-- Four rings, alternating red and white - `DrawCircle` fills a disc, so each
-- smaller ring is drawn over the one before it rather than the two nesting
-- as an outline would.
local RING_COLOURS = {
	Color3.fromRGB(200, 40, 40),
	Color3.new(1, 1, 1),
	Color3.fromRGB(200, 40, 40),
	Color3.new(1, 1, 1),
}
local CENTRE = Vector2.new(64, 64)

local function paintRings()
	for index, colour in RING_COLOURS do
		local radius = 60 - (index - 1) * 14
		image:DrawCircle(CENTRE, radius, colour)
	end
end

paintRings()

local floor = Instance.new("Part")
floor.Name = "Floor"
floor.Anchored = true
floor.Size = Vector3.new(24, 1, 24)
floor.Position = Vector3.new(0, -0.5, 0)
floor.Color = Color3.fromRGB(118, 120, 126)
floor.Parent = workspace

-- **A `MeshPart` and not a plain `Part`.** A `Part` is textured only through
-- a `Material` child - `scene/Materials.hpp` carries the whole argument for
-- why - so the direct property this scene wants, `TextureID`, is
-- `MeshPart`'s alone.
local part = Instance.new("MeshPart")
part.Name = "Target"
part.Anchored = true
part.Size = Vector3.new(4, 4, 0.2)
part.Position = Vector3.new(0, 2.5, 0)

-- **The one line that makes an `EditableImage` a thing this part shows.**
-- `TextureID` resolves a texture name the same way whether it names a
-- published sheet or, as here, an image nothing published ever touched -
-- `render::TextureTable` does not know or care which.
part.TextureID = image.ContentId
part.Parent = workspace

-- **Somewhere to stand, because the target is two tenths of a stud thick.**
-- With no camera the scene opened on the client's own starting eye, which looks
-- along the floor - so a four-by-four painted panel seen edge-on was a red
-- sliver a few pixels wide, and a picture of a red sliver reads as "the texture
-- did not apply". It had applied the whole time. Square to the front face, close
-- enough that the rings fill the frame.
local eye = Instance.new("Camera")
eye.Name = "Viewer"
eye.FieldOfView = 55
eye.CFrame = CFrame.lookAt(Vector3.new(0, 2.5, 7), Vector3.new(0, 2.5, 0))
eye.Parent = workspace
workspace.CurrentCamera = eye

-- **Live editing.** A crosshair sweeps across the rings on
-- `RunService.Heartbeat`, through `DrawLine` calls that bump `EditableImage.
-- Revision` exactly as the rings' own `DrawCircle` calls did once - proving
-- the "keeps editing after it is already on screen" half without needing a
-- second mechanism from the one `EditableMesh.luau` already demonstrates.
RunService.Heartbeat:Connect(function()
	paintRings()

	local angle = elapsedTime() * 1.5
	local reach = 60
	local offset = Vector2.new(math.cos(angle), math.sin(angle)) * reach
	image:DrawLine(CENTRE - offset, CENTRE + offset, Color3.new(0, 0, 0))
end)
