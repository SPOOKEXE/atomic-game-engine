--!strict
-- An arcade tornado scene driven entirely by one Luau wind field.
--
-- `docs/TORNADOSIM.md` records why this uses kinematic debris and existing GPU
-- emitters rather than a new tornado-specific engine system.

local Lighting = game:GetService("Lighting")
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local StarterGui = game:GetService("StarterGui")
local workspace = game:GetService("Workspace")

local GROUND_LIFT = 0.01
local MAXIMUM_DEBRIS = 144
local MAXIMUM_FRAME_SECONDS = 1 / 20
local GRAVITY = Vector3.new(0, -24, 0)
local FUNNEL_LAYER_COUNT = 34
local FUNNEL_HEIGHT = 156
local CAMERA_OFFSET = Vector3.new(106, 42, 128)

type Storm = {
	Centre: Vector3,
	BaseEnergy: number,
	Energy: number,
	CoreRadius: number,
	InfluenceRadius: number,
	PeakTangentialSpeed: number,
	PeakInflowSpeed: number,
	PeakUpdraftSpeed: number,
	Humidity: number,
	RainRate: number,
	Turbulence: number,
	Translation: Vector3,
	Tightness: number,
}

type FieldSample = {
	Velocity: Vector3,
	Influence: number,
	Hazard: number,
}

type Debris = {
	Part: Part,
	Home: Vector3,
	Position: Vector3,
	Velocity: Vector3,
	MassResponse: number,
	SpinRadians: number,
	Released: boolean,
}

type Tree = {
	Part: Part,
	Position: Vector3,
	YawRadians: number,
}

type Preset = {
	Name: string,
	Energy: number,
	CoreRadius: number,
	InfluenceRadius: number,
	PeakTangentialSpeed: number,
	PeakInflowSpeed: number,
	PeakUpdraftSpeed: number,
	Humidity: number,
	RainRate: number,
	Turbulence: number,
	Translation: Vector3,
	Tightness: number,
}

local presets: { Preset } = {
	{
		Name = "EF1",
		Energy = 0.42,
		CoreRadius = 18,
		InfluenceRadius = 120,
		PeakTangentialSpeed = 26,
		PeakInflowSpeed = 10,
		PeakUpdraftSpeed = 18,
		Humidity = 0.62,
		RainRate = 0.38,
		Turbulence = 4,
		Translation = Vector3.new(2, 0, 0.7),
		Tightness = 1.7,
	},
	{
		Name = "EF3",
		Energy = 0.72,
		CoreRadius = 28,
		InfluenceRadius = 185,
		PeakTangentialSpeed = 52,
		PeakInflowSpeed = 22,
		PeakUpdraftSpeed = 42,
		Humidity = 0.82,
		RainRate = 0.7,
		Turbulence = 11,
		Translation = Vector3.new(3.5, 0, 1.1),
		Tightness = 2.35,
	},
	{
		Name = "EF5",
		Energy = 1,
		CoreRadius = 42,
		InfluenceRadius = 300,
		PeakTangentialSpeed = 83,
		PeakInflowSpeed = 42,
		PeakUpdraftSpeed = 72,
		Humidity = 0.94,
		RainRate = 0.9,
		Turbulence = 21,
		Translation = Vector3.new(5, 0, 1.6),
		Tightness = 3.1,
	},
}

local storm: Storm = {
	Centre = Vector3.new(-74, 0, -42),
	BaseEnergy = 0,
	Energy = 0,
	CoreRadius = 0,
	InfluenceRadius = 0,
	PeakTangentialSpeed = 0,
	PeakInflowSpeed = 0,
	PeakUpdraftSpeed = 0,
	Humidity = 0,
	RainRate = 0,
	Turbulence = 0,
	Translation = Vector3.zero,
	Tightness = 0,
}

local activePreset = 2
local elapsedSeconds = 0
local nextLightningSeconds = 4
local lightningUntilSeconds = 0

local function clamp(value: number, low: number, high: number): number
	return math.max(low, math.min(high, value))
end

local function applyPreset(index: number)
	activePreset = index
	local preset = presets[index]
	storm.BaseEnergy = preset.Energy
	storm.Energy = preset.Energy
	storm.CoreRadius = preset.CoreRadius
	storm.InfluenceRadius = preset.InfluenceRadius
	storm.PeakTangentialSpeed = preset.PeakTangentialSpeed
	storm.PeakInflowSpeed = preset.PeakInflowSpeed
	storm.PeakUpdraftSpeed = preset.PeakUpdraftSpeed
	storm.Humidity = preset.Humidity
	storm.RainRate = preset.RainRate
	storm.Turbulence = preset.Turbulence
	storm.Translation = preset.Translation
	storm.Tightness = preset.Tightness
end

local function lifecycleEnergy(timeSeconds: number): number
	local cycleSeconds = timeSeconds % 90
	if cycleSeconds < 18 then
		return 0.25 + 0.75 * (cycleSeconds / 18)
	end
	if cycleSeconds < 63 then
		return 1
	end
	return math.max(0.22, 1 - (cycleSeconds - 63) / 27)
end

local function sampleField(position: Vector3): FieldSample
	local offset = position - storm.Centre
	local radius = Vector3.new(offset.X, 0, offset.Z).Magnitude
	local radial = radius > 0.001 and Vector3.new(offset.X / radius, 0, offset.Z / radius) or Vector3.xAxis
	local tangent = Vector3.new(-radial.Z, 0, radial.X)
	local influence = 1 / (1 + (radius / storm.InfluenceRadius) ^ storm.Tightness)
	local energyScale = math.sqrt(storm.Energy)
	local coreFraction = radius / storm.CoreRadius
	local tangential = storm.PeakTangentialSpeed * energyScale * (2 * coreFraction)
		/ (1 + coreFraction * coreFraction) * influence
	local inflowRing = radius / (storm.CoreRadius * 2.15)
	local inflow = storm.PeakInflowSpeed * energyScale * inflowRing * math.exp(1 - inflowRing) * influence
	local lift = storm.PeakUpdraftSpeed * energyScale * math.exp(-(radius / (storm.CoreRadius * 1.9)) ^ 2)
		* clamp(offset.Y / 16, 0.18, 1) * clamp(1 - (offset.Y / 150) ^ 2, 0, 1)
	local phase = elapsedSeconds * 0.8
	local turbulence = Vector3.new(
		math.noise(position.X * 0.05, position.Z * 0.05, phase),
		math.noise(position.Z * 0.04, position.Y * 0.05, phase + 11),
		math.noise(position.Y * 0.04, position.X * 0.05, phase - 7)
	) * (storm.Turbulence * influence)
	local velocity = tangent * tangential - radial * inflow + Vector3.new(0, lift, 0) + turbulence + storm.Translation
	local hazard = clamp((tangential * tangential + inflow * inflow) / (85 * 85), 0, 1)
	return { Velocity = velocity, Influence = influence, Hazard = hazard }
end

local function part(name: string, size: Vector3, position: Vector3, colour: Color3, anchored: boolean): Part
	local made = Instance.new("Part")
	made.Name = name
	made.CanCollide = false
	made.CanQuery = false
	made.Size = size
	made.CFrame = CFrame.new(position)
	made.Color = colour
	made.Anchored = anchored
	made.Parent = workspace
	return made
end

local sky = Instance.new("SkyboxCompute")
sky.Name = "Tornado Sky"
sky.GroundColor = Color3.fromRGB(20, 24, 29)
sky.HorizonColor = Color3.fromRGB(86, 93, 111)
sky.ZenithColor = Color3.fromRGB(13, 17, 25)
sky.StarDensity = 0
sky.SunSize = 0.15
sky.Parent = Lighting

local atmosphere = Instance.new("AtmosphereProcedural")
atmosphere.Name = "Tornado Haze"
atmosphere.Color = Color3.fromRGB(134, 143, 157)
atmosphere.Decay = Color3.fromRGB(59, 67, 79)
atmosphere.Density = 0.38
atmosphere.Haze = 2.1
atmosphere.Glare = 0
atmosphere.Samples = 16
atmosphere.Parent = Lighting

local clouds = Instance.new("CloudCompute")
clouds.Name = "Storm Cloud Deck"
clouds.Color = Color3.fromRGB(103, 112, 124)
clouds.Cover = 0.86
clouds.Density = 0.92
clouds.WindDirection = Vector2.new(1, 0.3)
clouds.WindSpeed = 24
clouds.CellSize = 0.34
clouds.Detail = 0.76
clouds.Height = 0.62
clouds.Thickness = 0.34
clouds.Steps = 32
clouds.Seed = 87
clouds.Parent = Lighting

local ground = part("Storm Plain", Vector3.new(760, 1, 760), Vector3.new(0, -0.5 + GROUND_LIFT, 0), Color3.fromRGB(49, 55, 47), true)
ground.CastShadow = false

-- The funnel is a normal scene vector field. The emitters only select it from
-- their ancestor, so they do not need tornado-specific force properties.
local funnelField = Instance.new("VectorField3D")
funnelField.Name = "Funnel Wind Field"
funnelField.CFrame = CFrame.new(storm.Centre + Vector3.new(0, FUNNEL_HEIGHT * 0.5, 0))
funnelField.Bounds = Vector3.new(128, FUNNEL_HEIGHT * 0.65, 128)
funnelField.Falloff = 18
funnelField.Axis = Vector3.yAxis
funnelField.LocalSpace = false
funnelField.Parent = workspace

-- This is a regular placed medium. It gives the funnel a dense core with
-- light attenuated through the same volume, while the particles remain its
-- visible debris rather than becoming a tornado-only cloud renderer.
local funnelVolumes = {
	{ height = 18, halfHeight = 20, radius = 9 },
	{ height = 48, halfHeight = 24, radius = 17 },
	{ height = 88, halfHeight = 31, radius = 33 },
	{ height = 130, halfHeight = 43, radius = 54 },
}
for index, band in funnelVolumes do
	local volume = Instance.new("Volume")
	volume.Name = "Funnel Condensation"
	volume.CFrame = CFrame.new(storm.Centre + Vector3.new(0, band.height, 0))
	volume.Bounds = Vector3.new(band.radius, band.halfHeight, band.radius)
	volume.Shape = Enum.VolumeShape.Ellipsoid
	volume.Color = Color3.fromRGB(201, 208, 216)
	volume.Density = 0.68
	volume.Extinction = 0.32
	volume.Falloff = 0.72
	volume.NoiseScale = 0.11
	volume.NoiseStrength = 0.92
	volume.Steps = 32
	volume.ShadowSteps = 10
	volume.Seed = 909 + index
	volume.Parent = workspace
end

local funnelHosts: { Part } = {}

local function makeFunnelEmitter(host: Instance, layer: number, size: number): ParticleEmitter
	local emitter = Instance.new("ParticleEmitter")
	emitter.Name = "Condensation"
	emitter.Rate = 150 + layer * 8
	emitter.MaxParticles = 620
	emitter.Lifetime = NumberRange.new(2.6, 4.2)
	emitter.Speed = NumberRange.new(0.4, 2.1)
	emitter.Drag = 1.65
	emitter.MaxSpeed = 46
	emitter.Acceleration = Vector3.zero
	emitter.RadialAcceleration = 0
	emitter.TangentialAcceleration = 0
	emitter.NoiseStrength = 1.25
	emitter.NoiseFrequency = 0.19
	emitter.NoiseScrollSpeed = 0.42
	emitter.LockedToPart = false
	emitter.SoftParticles = true
	emitter.Shape = Enum.ParticleEmitterShape.Sphere
	emitter.ShapeStyle = Enum.ParticleEmitterShapeStyle.Volume
	emitter.ShapeInOut = Enum.ParticleEmitterShapeInOut.Outward
	emitter.Size = NumberSequence.new({
		NumberSequenceKeypoint.new(0, size * 0.42),
		NumberSequenceKeypoint.new(0.65, size),
		NumberSequenceKeypoint.new(1, size * 0.72),
	})
	emitter.Transparency = NumberSequence.new({
		NumberSequenceKeypoint.new(0, 0.82),
		NumberSequenceKeypoint.new(0.75, 0.87),
		NumberSequenceKeypoint.new(1, 1),
	})
	emitter.Squash = NumberSequence.new(-0.52)
	emitter.Rotation = NumberRange.new(0, 360)
	emitter.RotSpeed = NumberRange.new(-18, 18)
	emitter.Color = ColorSequence.new(Color3.fromRGB(196, 203, 211))
	emitter.Parent = host
	return emitter
end

local function funnelRadius(height: number): number
	local fraction = clamp(height / FUNNEL_HEIGHT, 0, 1)
	return 3.5 + fraction ^ 1.58 * 50
end

local function placeFunnel(timeSeconds: number)
	for index, host in funnelHosts do
		local height = (index - 1) * FUNNEL_HEIGHT / (FUNNEL_LAYER_COUNT - 1)
		local radius = funnelRadius(height)
		local phase = index * 1.91 + timeSeconds * (0.19 + height * 0.0014)
		host.CFrame = CFrame.new(storm.Centre + Vector3.new(math.cos(phase) * radius, height, math.sin(phase) * radius))
	end
end

for index = 1, FUNNEL_LAYER_COUNT do
	local height = (index - 1) * FUNNEL_HEIGHT / (FUNNEL_LAYER_COUNT - 1)
	local radius = funnelRadius(height)
	local host = part("Funnel Layer", Vector3.new(radius * 0.2 + 1.5, 4.5, radius * 0.2 + 1.5), storm.Centre, Color3.new(1, 1, 1), true)
	host.Visible = false
	host.Parent = funnelField
	makeFunnelEmitter(host, index, 1.9 + radius * 0.075)
	funnelHosts[index] = host
end
placeFunnel(0)

local rainHost = part("Rain Curtain", Vector3.new(132, 28, 108), storm.Centre, Color3.new(1, 1, 1), true)
rainHost.Visible = false
local rain = Instance.new("ParticleEmitter")
rain.Name = "Rain"
rain.Rate = 820
rain.MaxParticles = 18000
rain.Lifetime = NumberRange.new(1.2, 1.7)
rain.Shape = Enum.ParticleEmitterShape.Box
rain.ShapeStyle = Enum.ParticleEmitterShapeStyle.Volume
rain.ShapeInOut = Enum.ParticleEmitterShapeInOut.Outward
rain.Speed = NumberRange.new(12, 18)
rain.Acceleration = Vector3.new(10, -38, 4)
rain.Drag = 0.06
rain.LockedToPart = false
rain.TangentialAcceleration = 0
rain.RadialAcceleration = 0
rain.NoiseStrength = 1.5
rain.NoiseFrequency = 0.7
rain.NoiseScrollSpeed = 1.2
rain.Size = NumberSequence.new(0.12, 0.05)
rain.Transparency = NumberSequence.new(0.2, 0.85)
rain.Color = ColorSequence.new(Color3.fromRGB(94, 122, 151))
rain.Parent = rainHost

local lightningTop = part("Lightning Top", Vector3.one, Vector3.zero, Color3.new(1, 1, 1), true)
lightningTop.Visible = false
local lightningBottom = part("Lightning Bottom", Vector3.one, Vector3.zero, Color3.new(1, 1, 1), true)
lightningBottom.Visible = false
local topPoint = Instance.new("Attachment")
topPoint.Parent = lightningTop
local bottomPoint = Instance.new("Attachment")
bottomPoint.Parent = lightningBottom
local lightning = Instance.new("Beam")
lightning.Name = "Storm Lightning"
lightning.Attachment0 = topPoint
lightning.Attachment1 = bottomPoint
lightning.Width0 = 1.4
lightning.Width1 = 0.25
lightning.CurveSize0 = 16
lightning.CurveSize1 = -12
lightning.Color = ColorSequence.new(Color3.fromRGB(222, 237, 255))
lightning.Enabled = false
lightning.Parent = lightningTop
local flash = Instance.new("PointLight")
flash.Name = "Lightning Flash"
flash.Brightness = 0
flash.Range = 120
flash.Color = Color3.fromRGB(204, 224, 255)
flash.Parent = lightningBottom

local trees: { Tree } = {}
for index = 1, 64 do
	local angle = index * 2.399
	local radius = 55 + (index % 8) * 26
	local position = Vector3.new(math.cos(angle) * radius, 8, math.sin(angle) * radius)
	local tree = part("Wind Tree", Vector3.new(2, 16, 2), position, Color3.fromRGB(46, 76, 45), true)
	table.insert(trees, { Part = tree, Position = position, YawRadians = angle })
end

local debris: { Debris } = {}
local buildingOrigins = {
	Vector3.new(-68, 0.5, -42),
	Vector3.new(-24, 0.5, -62),
	Vector3.new(34, 0.5, -8),
}
for index = 1, MAXIMUM_DEBRIS do
	local home: Vector3
	local size: number
	local response: number
	local colour: Color3
	if index <= 48 then
		local buildingIndex = math.floor((index - 1) / 16) + 1
		local fragmentIndex = (index - 1) % 16
		local column = fragmentIndex % 4
		local row = math.floor(fragmentIndex / 4)
		home = buildingOrigins[buildingIndex] + Vector3.new((column - 1.5) * 3.4, row * 2.4 + 1.2, 0)
		size = 1.5
		response = 0.62 + row * 0.08
		colour = Color3.fromRGB(113, 83, 58)
	else
		local angle = index * 2.399
		local radius = 34 + (index % 12) * 13
		home = Vector3.new(math.cos(angle) * radius, 1.2, math.sin(angle) * radius)
		size = 0.45 + (index % 4) * 0.24
		response = 0.45 + (index % 7) * 0.09
		colour = Color3.fromRGB(111, 86, 57)
	end
	local fragment = part("Storm Debris", Vector3.new(size * 2, size * 0.35, size), home, colour, true)
	table.insert(debris, {
		Part = fragment,
		Home = home,
		Position = home,
		Velocity = Vector3.zero,
		MassResponse = response,
		SpinRadians = index * 2.399,
		Released = false,
	})
end

local camera = Instance.new("Camera")
camera.Name = "Tornado Camera"
camera.FieldOfView = 68
camera.FarPlaneZ = 1600
camera.CFrame = CFrame.lookAt(storm.Centre + CAMERA_OFFSET, storm.Centre + Vector3.new(0, 56, 0))
camera.Parent = workspace
workspace.CurrentCamera = camera

local screen = Instance.new("ScreenGui")
screen.Name = "Tornado Controls"
local localPlayer = Players.LocalPlayer
if localPlayer then
	local playerGui = localPlayer:FindFirstChild("PlayerGui")
	assert(playerGui, "the local player has no PlayerGui")
	screen.Parent = playerGui
else
	screen.Parent = StarterGui
end
local panel = Instance.new("Frame")
panel.Name = "Panel"
panel.BackgroundColor3 = Color3.new(0, 0, 0)
panel.BackgroundTransparency = 0.12
panel.Size = UDim2.new(0, 268, 0, 146)
panel.Position = UDim2.new(0, 18, 0, 18)
panel.Parent = screen
local status = Instance.new("TextLabel")
status.Name = "Status"
status.BackgroundTransparency = 1
status.Position = UDim2.new(0, 10, 0, 8)
status.Size = UDim2.new(1, -20, 0, 70)
status.TextColor3 = Color3.new(1, 1, 1)
status.TextSize = 15
status.TextXAlignment = Enum.TextXAlignment.Left
status.TextYAlignment = Enum.TextYAlignment.Top
status.Parent = panel

for index, preset in presets do
	local button = Instance.new("TextButton")
	button.Name = preset.Name
	button.BackgroundColor3 = Color3.fromRGB(24, 31, 42)
	button.Position = UDim2.new(0, 10 + (index - 1) * 83, 0, 96)
	button.Size = UDim2.new(0, 78, 0, 34)
	button.Text = preset.Name
	button.TextColor3 = Color3.new(1, 1, 1)
	button.TextSize = 15
	button.Parent = panel
	button.Activated:Connect(function()
		applyPreset(index)
	end)
end

applyPreset(activePreset)

RunService.Heartbeat:Connect(function(deltaTime: number)
	local stepSeconds = math.min(math.max(deltaTime, 0), MAXIMUM_FRAME_SECONDS)
	elapsedSeconds += stepSeconds
	storm.Energy = storm.BaseEnergy * lifecycleEnergy(elapsedSeconds)
	storm.Centre += storm.Translation * stepSeconds
	if storm.Centre.X > 180 then
		storm.Centre = Vector3.new(-180, 0, -110)
	end

	local energyScale = math.sqrt(storm.Energy)
	funnelField.CFrame = CFrame.new(storm.Centre + Vector3.new(0, FUNNEL_HEIGHT * 0.5, 0))
	funnelField.Vector = Vector3.new(0, 4 + storm.PeakUpdraftSpeed * 0.35 * energyScale, 0)
	funnelField.Radial = -storm.PeakInflowSpeed * 0.65 * energyScale
	funnelField.Tangential = storm.PeakTangentialSpeed * 0.55 * energyScale

	placeFunnel(elapsedSeconds)
	camera.CFrame = CFrame.lookAt(storm.Centre + CAMERA_OFFSET, storm.Centre + Vector3.new(0, 56, 0))

	rainHost.CFrame = CFrame.new(storm.Centre + Vector3.new(18, 24, -16))
	rain.Rate = 260 + storm.RainRate * 820

	for _, tree in trees do
		local wind = sampleField(tree.Position)
		local sideways = Vector3.new(wind.Velocity.X, 0, wind.Velocity.Z)
		local bendRadians = math.min(sideways.Magnitude / 110, 0.48) * wind.Influence
		local lean = sideways.Magnitude > 0.01 and sideways.Unit or Vector3.xAxis
		local leanYaw = math.atan2(lean.Z, lean.X) - math.pi / 2
		tree.Part.CFrame = CFrame.new(tree.Position) * CFrame.Angles(0, leanYaw, bendRadians)
	end

	for _, item in debris do
		local field = sampleField(item.Position)
		if not item.Released and field.Hazard > item.MassResponse * 0.58 then
			item.Released = true
		end
		if item.Released then
			item.Velocity += (field.Velocity - item.Velocity) * (item.MassResponse * stepSeconds * 1.8)
			item.Velocity += GRAVITY * stepSeconds
			item.Position += item.Velocity * stepSeconds
			item.SpinRadians += (field.Velocity.Magnitude * 0.03 + 1) * stepSeconds
			if item.Position.Y < 0.5 then
				item.Position = Vector3.new(item.Position.X, 0.5, item.Position.Z)
				item.Velocity = Vector3.new(item.Velocity.X * 0.72, math.abs(item.Velocity.Y) * 0.34, item.Velocity.Z * 0.72)
			end
			if (item.Position - storm.Centre).Magnitude > storm.InfluenceRadius * 1.7 or item.Position.Y > 190 then
				item.Position = item.Home
				item.Velocity = Vector3.zero
				item.Released = false
			end
		end
		item.Part.CFrame = CFrame.new(item.Position) * CFrame.Angles(item.SpinRadians, item.SpinRadians * 0.37, 0)
	end

	if elapsedSeconds >= nextLightningSeconds then
		nextLightningSeconds = elapsedSeconds + 2.8 + math.noise(elapsedSeconds, 3, 7) * 1.1
		lightningUntilSeconds = elapsedSeconds + 0.16
		local strike = storm.Centre + Vector3.new(55, 0, -44)
		lightningTop.CFrame = CFrame.new(strike + Vector3.new(0, 135, 0))
		lightningBottom.CFrame = CFrame.new(strike)
	end
	local isLightning = elapsedSeconds < lightningUntilSeconds
	lightning.Enabled = isLightning
	flash.Brightness = isLightning and 18 or 0

	local cameraField = sampleField(camera.CFrame.Position)
	status.Text = string.format(
		"tornado sim   %s   life %.0f%%\nwind %.0f m/s   hazard %.0f%%\ndebris %d   gpu particles: funnel + rain",
		presets[activePreset].Name,
		storm.Energy / storm.BaseEnergy * 100,
		cameraField.Velocity.Magnitude,
		cameraField.Hazard * 100,
		#debris
	)
end)

print("tornadosim: pure luau field, particle weather and bounded kinematic debris")
