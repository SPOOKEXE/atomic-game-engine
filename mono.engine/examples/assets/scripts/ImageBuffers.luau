--!strict
-- Exercise bounded raw RGBA8, PNG and JPEG imports plus each texture consumer.
--
--     client --script ImageBuffers.luau
--
-- Encoded imports retain their inferred dimensions and sRGB tag. Raw RGBA8
-- uses the current size, so resize before importing its exact pixel payload.
-- Imports cap at 1920x1080 (8,294,400 raw bytes) and 11,059,200 Base64 chars.

local PNG =
	"iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAYAAAD0In+KAAAAEUlEQVR4nGNocFCo/8/A8B8ADf8DXk5zibEAAAAASUVORK5CYII="
local JPEG =
	"/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAIBAQEBAQIBAQECAgICAgQDAgICAgUEBAMEBgUGBgYFBgYGBwkIBgcJBwYGCAsICQoKCgoKBggLDAsKDAkKCgr/wAALCAAQABABAREA/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/9oACAEBAAA/APi+v0or8G6/sUr/2Q=="
local BASE64_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"

local function Base64Value(character: string): number
	local value = string.find(BASE64_ALPHABET, character, 1, true)
	assert(value, "invalid base64 fixture")
	return value - 1
end

local function DecodeFixture(encoded: string): buffer
	local byteCount = math.floor(#encoded * 3 / 4)
	if string.sub(encoded, -2) == "==" then
		byteCount -= 2
	elseif string.sub(encoded, -1) == "=" then
		byteCount -= 1
	end
	local output = buffer.create(byteCount)
	local writeOffset = 0
	for offset = 1, #encoded, 4 do
		local thirdCharacter = string.sub(encoded, offset + 2, offset + 2)
		local fourthCharacter = string.sub(encoded, offset + 3, offset + 3)
		local third = if thirdCharacter == "=" then 0 else Base64Value(thirdCharacter)
		local fourth = if fourthCharacter == "=" then 0 else Base64Value(fourthCharacter)
		local packed = bit32.bor(
			bit32.lshift(Base64Value(string.sub(encoded, offset, offset)), 18),
			bit32.lshift(Base64Value(string.sub(encoded, offset + 1, offset + 1)), 12),
			bit32.lshift(third, 6),
			fourth
		)
		for shift = 16, 0, -8 do
			if writeOffset < byteCount then
				buffer.writeu8(output, writeOffset, bit32.band(bit32.rshift(packed, shift), 0xFF))
				writeOffset += 1
			end
		end
	end
	return output
end

local function NewImage(name: string): EditableImage
	local image = Instance.new("EditableImage")
	image.Name = name
	return image
end

local rawSource = NewImage("RawSource")
rawSource:Resize(64, 64)
rawSource.ColorSpace = "srgb"
assert(
	rawSource:DrawRectangle(Vector2.new(0, 0), Vector2.new(64, 64), Color3.fromRGB(28, 54, 88)),
	"raw source fill failed"
)
assert(rawSource:DrawCircle(Vector2.new(32, 32), 22, Color3.fromRGB(240, 160, 64)), "raw source mark failed")

local rawBuffer = NewImage("RawBuffer")
rawBuffer:Resize(64, 64)
rawBuffer.ColorSpace = rawSource.ColorSpace
assert(rawBuffer:FromBase64(rawSource:ToBase64(), "rgba8"), "raw RGBA8 export round-trip failed")

local pngBase64 = NewImage("PngBase64")
assert(pngBase64:FromBase64(PNG, "png"), "PNG Base64 import failed")

local pngBuffer = NewImage("PngBuffer")
assert(pngBuffer:FromEncodedBuffer(DecodeFixture(PNG), "png"), "PNG buffer import failed")

local jpegBase64 = NewImage("JpegBase64")
assert(jpegBase64:FromBase64(JPEG, "jpeg"), "JPEG Base64 import failed")

local jpegBuffer = NewImage("JpegBuffer")
assert(jpegBuffer:FromEncodedBuffer(DecodeFixture(JPEG), "jpeg"), "JPEG buffer import failed")

local variants = {
	{ Name = "RGBA export", Image = rawBuffer },
	{ Name = "PNG Base64", Image = pngBase64 },
	{ Name = "PNG buffer", Image = pngBuffer },
	{ Name = "JPEG Base64", Image = jpegBase64 },
	{ Name = "JPEG buffer", Image = jpegBuffer },
}

local screen = Instance.new("ScreenGui")
screen.Name = "ImageBuffers"
local localPlayer = game:GetService("Players").LocalPlayer
if localPlayer then
	local playerGui = localPlayer:FindFirstChild("PlayerGui")
	assert(playerGui, "the local player has no PlayerGui")
	screen.Parent = playerGui
else
	screen.Parent = game:GetService("StarterGui")
end

local panel = Instance.new("Frame")
panel.Name = "ImageBufferGallery"
panel.Position = UDim2.new(0.5, -260, 0, 16)
panel.Size = UDim2.new(0, 520, 0, 144)
panel.BackgroundColor3 = Color3.new(0, 0, 0)
panel.BackgroundTransparency = 0.12
panel.Parent = screen

local title = Instance.new("TextLabel")
title.Size = UDim2.new(1, -16, 0, 24)
title.Position = UDim2.new(0, 8, 0, 4)
title.BackgroundTransparency = 1
title.Text = "EditableImage: bounded raw / PNG / JPEG"
title.TextColor3 = Color3.new(1, 1, 1)
title.TextXAlignment = Enum.TextXAlignment.Left
title.Parent = panel

for index, variant in variants do
	local imageLabel = Instance.new("ImageLabel")
	imageLabel.Name = variant.Name
	imageLabel.Position = UDim2.new(0, 12 + (index - 1) * 100, 0, 32)
	imageLabel.Size = UDim2.new(0, 88, 0, 84)
	imageLabel.BackgroundColor3 = Color3.fromRGB(24, 24, 24)
	imageLabel.ScaleType = Enum.ScaleType.Fit
	imageLabel.Image = variant.Image.ContentId
	imageLabel.Parent = panel

	local caption = Instance.new("TextLabel")
	caption.Position = UDim2.new(0, 12 + (index - 1) * 100, 0, 118)
	caption.Size = UDim2.new(0, 88, 0, 18)
	caption.BackgroundTransparency = 1
	caption.Text = variant.Name
	caption.TextColor3 = Color3.fromRGB(210, 210, 210)
	caption.TextSize = 11
	caption.Parent = panel
end

local floor = Instance.new("Part")
floor.Name = "ImageBufferFloor"
floor.Anchored = true
floor.Size = Vector3.new(18, 1, 12)
floor.Position = Vector3.new(0, -0.51, 0)
floor.Color = Color3.fromRGB(28, 28, 30)
floor.Parent = workspace

local meshPart = Instance.new("MeshPart")
meshPart.Name = "PngMeshPart"
meshPart.MeshId = "engine.Cube"
meshPart.TextureID = pngBuffer.ContentId
meshPart.Anchored = true
meshPart.Size = Vector3.new(3, 3, 0.4)
meshPart.Position = Vector3.new(-3.5, 1.5, 0)
meshPart.Parent = workspace

local texturedPart = Instance.new("Part")
texturedPart.Name = "RawTexturePart"
texturedPart.Anchored = true
texturedPart.Size = Vector3.new(3, 3, 0.4)
texturedPart.Position = Vector3.new(0, 1.5, 0)
texturedPart.Color = Color3.fromRGB(255, 255, 255)
texturedPart.Parent = workspace

local texture = Instance.new("Texture")
texture.Name = "RawRGBA"
texture.Face = Enum.NormalId.Back
texture.Texture = rawBuffer.ContentId
texture.Parent = texturedPart

local particleHost = Instance.new("Part")
particleHost.Name = "JpegParticleHost"
particleHost.Anchored = true
particleHost.Size = Vector3.new(3, 1, 3)
particleHost.Position = Vector3.new(3.5, 0.5, 0)
particleHost.Color = Color3.fromRGB(40, 40, 44)
particleHost.Parent = workspace

local emitter = Instance.new("ParticleEmitter")
emitter.Name = "JpegParticles"
emitter.Texture = jpegBuffer.ContentId
emitter.Rate = 8
emitter.Lifetime = NumberRange.new(2, 3)
emitter.Speed = NumberRange.new(1, 2)
emitter.SpreadAngle = Vector2.new(25, 25)
emitter.Size = NumberSequence.new(0.7)
emitter.Parent = particleHost

local camera = Instance.new("Camera")
camera.Name = "ImageBuffersCamera"
camera.CFrame = CFrame.lookAt(Vector3.new(0, 3.5, 12), Vector3.new(0, 1.5, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera
