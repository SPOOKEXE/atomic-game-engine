--!strict
-- A focused image-particle and flipbook proof scene.
--
-- Run with the store prepared by `scripts/demos/bake-fox-flipbook.sh`:
--
--   client --cdn dir:.cache/build/dev/captures/fox-flipbook/store \\
--     --cdn dir:$HOME/Documents/atomic-game-engine/cdn/processed \\
--     --script ParticleFlipbooks.luau
--
-- Every bay names the same real GIF bake. The only difference is playback:
-- one-shot, loop, ping-pong, and random fixed cells. Loop and PingPong begin
-- each fox on a deterministic random phase. The check at the bottom
-- waits for the texture facts, so it proves both that the particle requested
-- the image and that the baked flipbook metadata made it through the client.

local RunService = game:GetService("RunService")
local TEXTURE = "effects/fox_dance.atex"
local BAY_SPACING = 11

local floor = Instance.new("Part")
floor.Name = "FlipbookFloor"
floor.Anchored = true
floor.Size = Vector3.new(54, 1, 22)
floor.CFrame = CFrame.new(0, -0.5, 0)
floor.Color = Color3.new(0.025, 0.03, 0.05)
floor.Parent = workspace

-- This creates a wide, shallow spawn strip. Several foxes can be read beside
-- one another instead of sitting exactly on top of each other at the emitter.
local function makeBay(
	index: number,
	name: string,
	mode: Enum.ParticleFlipbookMode,
	colour: Color3,
	randomStart: boolean,
	framerate: number
)
	local x = (index - 2.5) * BAY_SPACING
	local host = Instance.new("Part")
	host.Name = name
	host.Anchored = true
	host.Size = Vector3.new(8, 0.25, 0.8)
	host.CFrame = CFrame.new(x, 0.125, 0)
	host.Color = colour
	host.Parent = workspace

	local emitter = Instance.new("ParticleEmitter")
	emitter.Name = name .. "Foxes"
	emitter.Texture = TEXTURE
	emitter.FlipbookLayout = Enum.ParticleFlipbookLayout.Grid8x8
	emitter.FlipbookMode = mode
	emitter.FlipbookStartRandom = randomStart
	-- Zero means use the baked frame count. The fox GIF has 48 frames in an 8x8
	-- sheet, so the sixteen blank cells must stay unused.
	emitter.FlipbookFrames = 0
	if framerate > 0 then
		emitter.FlipbookFramerate = NumberRange.new(framerate, framerate)
	end
	emitter.Rate = 1.2
	emitter.Lifetime = NumberRange.new(4, 4)
	emitter.Speed = NumberRange.new(0.35, 0.35)
	emitter.SpreadAngle = Vector2.new(0, 0)
	emitter.Acceleration = Vector3.new(0, 0.25, 0)
	emitter.Orientation = Enum.ParticleOrientation.FacingCamera
	emitter.Size = NumberSequence.new(4.25)
	emitter.Transparency = NumberSequence.new(0)
	emitter.Color = ColorSequence.new(Color3.new(1, 1, 1))
	emitter.Parent = host

	return emitter
end

makeBay(1, "OneShot", Enum.ParticleFlipbookMode.OneShot, Color3.new(0.85, 0.32, 0.18), false, 0)
makeBay(2, "Loop", Enum.ParticleFlipbookMode.Loop, Color3.new(0.18, 0.62, 0.95), true, 6)
makeBay(3, "PingPong", Enum.ParticleFlipbookMode.PingPong, Color3.new(0.34, 0.9, 0.4), true, 6)
makeBay(4, "Random", Enum.ParticleFlipbookMode.Random, Color3.new(0.82, 0.42, 0.92), false, 0)

local camera = Instance.new("Camera")
camera.Name = "FlipbookCamera"
camera.FieldOfView = 62
camera.FarPlaneZ = 500
camera.CFrame = CFrame.lookAt(Vector3.new(0, 9, 30), Vector3.new(0, 4, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

local checked = false
RunService.Heartbeat:Connect(function()
	if checked then
		return
	end

	local facts = ContentService:GetFlipbook(TEXTURE)
	if facts == nil then
		return
	end

	print(`particle flipbooks: {TEXTURE} says {facts.Side}x{facts.Side}, {facts.Frames} frames at {facts.FrameRate} fps`)
	assert(facts.Side == 8, "fox flipbook must be an 8x8 sheet")
	assert(facts.Frames == 48, "fox flipbook must use 48 nonblank cells")
	assert(math.abs(facts.FrameRate - 24) < 0.001, "fox flipbook must retain its 24 fps bake")
	checked = true
	print(`particle flipbooks: loaded {TEXTURE}, {facts.Frames} frames at {facts.FrameRate} fps`)
end)
