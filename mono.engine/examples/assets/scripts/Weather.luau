--!strict
-- A compact weather playground for the lighting and participating-media path.
--
-- The panels drive target values. Heartbeat blends the targets into the live
-- environment so a preset change reads as weather arriving, instead of as a
-- frame of unrelated property changes. Fog stays local: the script does not
-- write Lighting.FogStart, Lighting.FogEnd or Lighting.FogColor.

local Lighting = game:GetService("Lighting")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local StarterGui = game:GetService("StarterGui")
local Workspace = game:GetService("Workspace")

local MAXIMUM_FRAME_SECONDS = 1 / 20
local LIGHTNING_INTERVAL_SECONDS = 5.2

type Weather = {
	Name: string,
	Brightness: number,
	ClockTime: number,
	Ambient: Color3,
	OutdoorAmbient: Color3,
	BloomIntensity: number,
	DepthOfFieldIntensity: number,
	GodRayIntensity: number,
	CloudCover: number,
	CloudDensity: number,
	CloudWindSpeed: number,
	AtmosphereDensity: number,
	AtmosphereHaze: number,
	AtmosphereGlare: number,
	RainRate: number,
	RainSpeed: number,
	FogDensity: number,
	FogExtinction: number,
}

local presets: { Weather } = {
	{
		Name = "CLEAR",
		Brightness = 3.0,
		ClockTime = 16.2,
		Ambient = Color3.fromRGB(44, 51, 68),
		OutdoorAmbient = Color3.fromRGB(110, 132, 170),
		BloomIntensity = 0.72,
		DepthOfFieldIntensity = 0.32,
		GodRayIntensity = 0.42,
		CloudCover = 0.22,
		CloudDensity = 0.35,
		CloudWindSpeed = 4,
		AtmosphereDensity = 0.16,
		AtmosphereHaze = 0.55,
		AtmosphereGlare = 0.52,
		RainRate = 0,
		RainSpeed = 18,
		FogDensity = 0.05,
		FogExtinction = 0.2,
	},
	{
		Name = "RAIN",
		Brightness = 1.35,
		ClockTime = 14.4,
		Ambient = Color3.fromRGB(24, 30, 42),
		OutdoorAmbient = Color3.fromRGB(57, 73, 104),
		BloomIntensity = 1.0,
		DepthOfFieldIntensity = 0.54,
		GodRayIntensity = 0.68,
		CloudCover = 0.74,
		CloudDensity = 0.76,
		CloudWindSpeed = 13,
		AtmosphereDensity = 0.34,
		AtmosphereHaze = 1.8,
		AtmosphereGlare = 0.12,
		RainRate = 280,
		RainSpeed = 28,
		FogDensity = 0.19,
		FogExtinction = 0.55,
	},
	{
		Name = "TEMPEST",
		Brightness = 0.55,
		ClockTime = 19.3,
		Ambient = Color3.fromRGB(10, 14, 25),
		OutdoorAmbient = Color3.fromRGB(25, 37, 64),
		BloomIntensity = 1.8,
		DepthOfFieldIntensity = 0.72,
		GodRayIntensity = 0.9,
		CloudCover = 0.96,
		CloudDensity = 0.96,
		CloudWindSpeed = 24,
		AtmosphereDensity = 0.52,
		AtmosphereHaze = 3.4,
		AtmosphereGlare = 0.02,
		RainRate = 460,
		RainSpeed = 38,
		FogDensity = 0.37,
		FogExtinction = 0.9,
	},
}

local function lerp(left: number, right: number, alpha: number): number
	return left + (right - left) * alpha
end

local function lerpColour(left: Color3, right: Color3, alpha: number): Color3
	return Color3.new(
		lerp(left.R, right.R, alpha),
		lerp(left.G, right.G, alpha),
		lerp(left.B, right.B, alpha)
	)
end

local function makePart(name: string, size: Vector3, position: Vector3, colour: Color3): Part
	local part = Instance.new("Part")
	part.Name = name
	part.Anchored = true
	part.Size = size
	part.Position = position
	part.Color = colour
	part.Parent = Workspace
	return part
end

Lighting.BloomThreshold = 0.8
Lighting.BloomRadius = 8
Lighting.BloomIntensity = 1.0
Lighting.DepthOfFieldFocusDistance = 54
Lighting.DepthOfFieldFocusRange = 14
Lighting.DepthOfFieldRadius = 7
Lighting.DepthOfFieldIntensity = 0.54
Lighting.GodRayThreshold = 0.72
Lighting.GodRayRadius = 72
Lighting.GodRayIntensity = 0.68

local sky = Instance.new("SkyboxCompute")
sky.Name = "Weather Sky"
sky.Shader = Enum.SkyboxComputeShader.Sunset
sky.SunSize = 0.075
sky.Seed = 77
sky.Parent = Lighting

local atmosphere = Instance.new("AtmosphereProcedural")
atmosphere.Name = "Weather Atmosphere"
atmosphere.Shader = Enum.AtmosphereProceduralShader.Earth
atmosphere.Color = Color3.fromRGB(116, 142, 176)
atmosphere.Decay = Color3.fromRGB(65, 79, 112)
atmosphere.Samples = 16
atmosphere.Parent = Lighting

local clouds = Instance.new("CloudCompute")
clouds.Name = "Weather Clouds"
clouds.Shader = Enum.CloudComputeShader.Storm
clouds.Color = Color3.fromRGB(128, 141, 160)
clouds.WindDirection = Vector2.new(0.9, 0.3)
clouds.CellSize = 0.24
clouds.Detail = 0.82
clouds.Height = 0.62
clouds.Thickness = 0.42
clouds.Steps = 32
clouds.Seed = 1337
clouds.Parent = Lighting

makePart("Weather Ground", Vector3.new(150, 1, 150), Vector3.new(0, -0.5, -28), Color3.fromRGB(26, 31, 38))
makePart("Weather Ruin Left", Vector3.new(12, 18, 8), Vector3.new(-22, 9, -38), Color3.fromRGB(53, 57, 63))
makePart("Weather Ruin Right", Vector3.new(9, 26, 9), Vector3.new(22, 13, -56), Color3.fromRGB(47, 52, 61))
makePart("Weather Beacon", Vector3.new(4, 20, 4), Vector3.new(0, 10, -72), Color3.fromRGB(36, 42, 51))

-- `Bounds` is a half-extent. The Y half-extent is intentionally small, making
-- this a squashed ellipsoid of mist along the ground instead of world fog.
local lowBank = Instance.new("Volume")
lowBank.Name = "Fog Field Low Bank"
lowBank.Position = Vector3.new(-14, 2.8, -32)
lowBank.Bounds = Vector3.new(30, 2.8, 16)
lowBank.Shape = Enum.VolumeShape.Ellipsoid
lowBank.Color = Color3.fromRGB(157, 178, 201)
lowBank.Falloff = 0.82
lowBank.NoiseScale = 0.16
lowBank.NoiseStrength = 0.74
lowBank.Steps = 28
lowBank.ShadowSteps = 8
lowBank.Seed = 29
lowBank.Parent = Workspace

local ruinMist = Instance.new("Volume")
ruinMist.Name = "Fog Field Ruin Veil"
ruinMist.Position = Vector3.new(23, 7, -55)
ruinMist.Bounds = Vector3.new(12, 9, 12)
ruinMist.Shape = Enum.VolumeShape.Ellipsoid
ruinMist.Color = Color3.fromRGB(130, 153, 184)
ruinMist.Falloff = 0.64
ruinMist.NoiseScale = 0.24
ruinMist.NoiseStrength = 0.86
ruinMist.Steps = 32
ruinMist.ShadowSteps = 12
ruinMist.Seed = 71
ruinMist.Parent = Workspace

local rainEmitters: { ParticleEmitter } = {}
for x = -1, 1 do
	for z = -1, 1 do
		-- ParticleEmitter has no independent world-size property. A 3x3 host grid
		-- keeps the weather local to the play space while covering the camera view.
		local rainHost = makePart(
			"Weather Rain Host",
			Vector3.new(1, 1, 1),
			Vector3.new(x * 18, 30, -30 + z * 18),
			Color3.new(1, 1, 1)
		)
		rainHost.Visible = false
		local rain = Instance.new("ParticleEmitter")
		rain.Name = "Weather Rain"
		rain.Rate = 0
		rain.MaxParticles = 360
		rain.Lifetime = NumberRange.new(1.1, 1.55)
		rain.Speed = NumberRange.new(18, 28)
		rain.Acceleration = Vector3.new(7, -46, 3)
		rain.Drag = 0.08
		rain.Shape = Enum.ParticleEmitterShape.Box
		rain.ShapeStyle = Enum.ParticleEmitterShapeStyle.Volume
		rain.ShapeInOut = Enum.ParticleEmitterShapeInOut.Outward
		rain.ShapePartial = 1
		rain.SpreadAngle = Vector2.new(7, 7)
		rain.Size = NumberSequence.new(0.07, 0.04)
		rain.Squash = NumberSequence.new(-0.75, -0.9)
		rain.Transparency = NumberSequence.new(0.3, 0.92)
		rain.Color = ColorSequence.new(Color3.fromRGB(159, 185, 214))
		rain.NoiseStrength = 0.65
		rain.NoiseFrequency = 0.9
		rain.NoiseScrollSpeed = 1.4
		rain.SoftParticles = true
		rain.Parent = rainHost
		table.insert(rainEmitters, rain)
	end
end

local lightningTop = makePart("Weather Lightning Top", Vector3.one, Vector3.new(18, 88, -48), Color3.new(1, 1, 1))
lightningTop.Visible = false
local lightningBottom = makePart("Weather Lightning Bottom", Vector3.one, Vector3.new(4, 1, -48), Color3.new(1, 1, 1))
lightningBottom.Visible = false
local lightningStart = Instance.new("Attachment")
lightningStart.Parent = lightningTop
local lightningEnd = Instance.new("Attachment")
lightningEnd.Parent = lightningBottom
local lightning = Instance.new("Beam")
lightning.Name = "Weather Lightning"
lightning.Attachment0 = lightningStart
lightning.Attachment1 = lightningEnd
lightning.Width0 = 1.1
lightning.Width1 = 0.18
lightning.CurveSize0 = 12
lightning.CurveSize1 = -8
lightning.Color = ColorSequence.new(Color3.fromRGB(218, 234, 255))
lightning.Enabled = false
lightning.Parent = lightningTop
local lightningFlash = Instance.new("PointLight")
lightningFlash.Name = "Weather Lightning Flash"
lightningFlash.Color = Color3.fromRGB(201, 222, 255)
lightningFlash.Range = 110
lightningFlash.Brightness = 0
lightningFlash.Parent = lightningBottom

local camera = Instance.new("Camera")
camera.Name = "Weather Camera"
camera.FieldOfView = 62
camera.FarPlaneZ = 550
camera.CFrame = CFrame.lookAt(Vector3.new(43, 18, 42), Vector3.new(0, 7, -42))
camera.Parent = Workspace
Workspace.CurrentCamera = camera

local screen = Instance.new("ScreenGui")
screen.Name = "Weather Controls"
local localPlayer = Players.LocalPlayer
if localPlayer then
	local playerGui = localPlayer:FindFirstChild("PlayerGui")
	assert(playerGui, "the local player has no PlayerGui")
	screen.Parent = playerGui
else
	screen.Parent = StarterGui
end

local panel = Instance.new("Frame")
panel.Name = "Weather Panel"
panel.BackgroundColor3 = Color3.new(0, 0, 0)
panel.BackgroundTransparency = 0.08
panel.BorderSizePixel = 0
panel.Position = UDim2.new(0, 18, 0, 18)
panel.Size = UDim2.new(0, 310, 0, 394)
panel.Parent = screen

local status = Instance.new("TextLabel")
status.Name = "Weather Status"
status.BackgroundTransparency = 1
status.Position = UDim2.new(0, 12, 0, 10)
status.Size = UDim2.new(1, -24, 0, 58)
status.TextColor3 = Color3.new(1, 1, 1)
status.TextSize = 15
status.TextWrapped = true
status.TextXAlignment = Enum.TextXAlignment.Left
status.TextYAlignment = Enum.TextYAlignment.Top
status.Parent = panel

local activePreset = 2
local target = presets[activePreset]
local fogMode = "LOW BANK"
local fogSize = 1
local fogSquash = 1
local fogFluff = 1

local function applyFogProfile()
	local sizeScale = ({ 0.68, 1, 1.36 })[fogSize]
	local squashScale = ({ 0.45, 1, 1.75 })[fogSquash]
	local fluffiness = ({ 0.28, 0.74, 1 })[fogFluff]
	lowBank.Bounds = Vector3.new(30 * sizeScale, 2.8 * squashScale, 16 * sizeScale)
	lowBank.NoiseStrength = fluffiness
	lowBank.NoiseScale = if fogFluff == 3 then 0.11 else 0.16
end

local function updateStatus()
	status.Text = string.format(
		"WEATHER  %s\nFOG  %s  |  local volumes, no global fog\nRAIN %.0f/s  CLOUD %.0f%%\nDOF %.0f%%  RAYS %.0f%%",
		target.Name,
		fogMode,
		target.RainRate,
		target.CloudCover * 100,
		target.DepthOfFieldIntensity * 100,
		target.GodRayIntensity * 100
	)
end

local function button(name: string, text: string, column: number, row: number): TextButton
	local control = Instance.new("TextButton")
	control.Name = name
	control.BackgroundColor3 = Color3.fromRGB(25, 35, 50)
	control.BorderSizePixel = 0
	control.Position = UDim2.new(0, 12 + column * 94, 0, 78 + row * 48)
	control.Size = UDim2.new(0, 86, 0, 36)
	control.Text = text
	control.TextColor3 = Color3.new(1, 1, 1)
	control.TextSize = 13
	control.Parent = panel
	return control
end

for index, preset in presets do
	local control = button("Weather " .. preset.Name, preset.Name, index - 1, 0)
	control.Activated:Connect(function()
		activePreset = index
		target = presets[activePreset]
		updateStatus()
	end)
end

local lowBankControl = button("Fog Low Bank", "LOW BANK", 0, 1)
lowBankControl.Activated:Connect(function()
	fogMode = "LOW BANK"
	lowBank.Shape = Enum.VolumeShape.Ellipsoid
	fogSize = 1
	fogSquash = 1
	fogFluff = 2
	applyFogProfile()
	updateStatus()
end)

local squashedControl = button("Fog Squashed", "SQUASHED", 1, 1)
squashedControl.Activated:Connect(function()
	fogMode = "SQUASHED"
	lowBank.Shape = Enum.VolumeShape.Ellipsoid
	fogSize = 3
	fogSquash = 1
	fogFluff = 1
	applyFogProfile()
	updateStatus()
end)

local fluffyControl = button("Fog Fluffy", "FLUFFY", 2, 1)
fluffyControl.Activated:Connect(function()
	fogMode = "FLUFFY"
	fogFluff = 3
	applyFogProfile()
	updateStatus()
end)

local function slider(name: string, label: string, row: number): TextButton
	local track = Instance.new("TextButton")
	track.Name = name
	track.BackgroundColor3 = Color3.fromRGB(25, 35, 50)
	track.BorderSizePixel = 0
	track.Position = UDim2.new(0, 12, 0, 184 + row * 48)
	track.Size = UDim2.new(0, 282, 0, 36)
	track.Text = label
	track.TextColor3 = Color3.new(1, 1, 1)
	track.TextSize = 13
	track.Parent = panel
	return track
end

-- Clicking a slider advances through its three deliberate visual states. The
-- discrete positions make the three volume properties easy to compare in a
-- capture, while retaining a compact control surface for the demo.
local sizeSlider = slider("Fog Size Slider", "SIZE     100%", 0)
local squashSlider = slider("Fog Squash Slider", "SQUASH   100%", 1)
local fluffSlider = slider("Fog Fluffy Slider", "FLUFFY   100%", 2)

local function advanceSlider(value: number): number
	return value % 3 + 1
end

sizeSlider.Activated:Connect(function()
	fogSize = advanceSlider(fogSize)
	sizeSlider.Text = string.format("SIZE     %.0f%%", ({ 68, 100, 136 })[fogSize])
	applyFogProfile()
end)

squashSlider.Activated:Connect(function()
	fogSquash = advanceSlider(fogSquash)
	squashSlider.Text = string.format("SQUASH   %.0f%%", ({ 45, 100, 175 })[fogSquash])
	applyFogProfile()
end)

fluffSlider.Activated:Connect(function()
	fogFluff = advanceSlider(fogFluff)
	fluffSlider.Text = string.format("FLUFFY   %.0f%%", ({ 28, 74, 100 })[fogFluff])
	applyFogProfile()
end)

local shapeSlider = slider("Fog Shape Slider", "SHAPE    ELLIPSOID", 3)
shapeSlider.Activated:Connect(function()
	if lowBank.Shape == Enum.VolumeShape.Ellipsoid then
		lowBank.Shape = Enum.VolumeShape.Box
		shapeSlider.Text = "SHAPE    BOX"
	else
		lowBank.Shape = Enum.VolumeShape.Ellipsoid
		shapeSlider.Text = "SHAPE    ELLIPSOID"
	end
end)

updateStatus()

local elapsedSeconds = 0
local nextLightningSeconds = LIGHTNING_INTERVAL_SECONDS
local lightningUntilSeconds = 0

RunService.Heartbeat:Connect(function(deltaTime: number)
	local stepSeconds = math.min(math.max(deltaTime, 0), MAXIMUM_FRAME_SECONDS)
	local alpha = math.min(stepSeconds * 0.7, 1)
	elapsedSeconds += stepSeconds

	Lighting.Brightness = lerp(Lighting.Brightness, target.Brightness, alpha)
	Lighting.ClockTime = lerp(Lighting.ClockTime, target.ClockTime, alpha)
	Lighting.Ambient = lerpColour(Lighting.Ambient, target.Ambient, alpha)
	Lighting.OutdoorAmbient = lerpColour(Lighting.OutdoorAmbient, target.OutdoorAmbient, alpha)
	Lighting.BloomIntensity = lerp(Lighting.BloomIntensity, target.BloomIntensity, alpha)
	Lighting.DepthOfFieldIntensity = lerp(
		Lighting.DepthOfFieldIntensity,
		target.DepthOfFieldIntensity,
		alpha
	)
	Lighting.GodRayIntensity = lerp(Lighting.GodRayIntensity, target.GodRayIntensity, alpha)
	atmosphere.Density = lerp(atmosphere.Density, target.AtmosphereDensity, alpha)
	atmosphere.Haze = lerp(atmosphere.Haze, target.AtmosphereHaze, alpha)
	atmosphere.Glare = lerp(atmosphere.Glare, target.AtmosphereGlare, alpha)
	clouds.Cover = lerp(clouds.Cover, target.CloudCover, alpha)
	clouds.Density = lerp(clouds.Density, target.CloudDensity, alpha)
	clouds.WindSpeed = lerp(clouds.WindSpeed, target.CloudWindSpeed, alpha)
	for _, rain in rainEmitters do
		rain.Rate = lerp(rain.Rate, target.RainRate / #rainEmitters, alpha)
		rain.Speed = NumberRange.new(target.RainSpeed * 0.8, target.RainSpeed)
	end
	lowBank.Density = lerp(lowBank.Density, target.FogDensity, alpha)
	ruinMist.Density = lerp(ruinMist.Density, target.FogDensity * 1.3, alpha)
	lowBank.Extinction = lerp(lowBank.Extinction, target.FogExtinction, alpha)
	ruinMist.Extinction = lerp(ruinMist.Extinction, target.FogExtinction * 1.15, alpha)

	if target.Name == "TEMPEST" and elapsedSeconds >= nextLightningSeconds then
		nextLightningSeconds = elapsedSeconds + LIGHTNING_INTERVAL_SECONDS
		lightningUntilSeconds = elapsedSeconds + 0.15
		lightning.Enabled = true
	end

	local flashing = elapsedSeconds < lightningUntilSeconds
	lightning.Enabled = flashing
	lightningFlash.Brightness = if flashing then 550 else 0
end)
