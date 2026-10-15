--!strict
-- A compact weather playground for authored clouds, weather and local fog.
--
-- The panels drive target values. Heartbeat blends the targets into the live
-- environment so a preset change reads as weather arriving, instead of as a
-- frame of unrelated property changes. The weather provider drives precipitation
-- and storm events; authored fog volumes add local appearance controls.

local Lighting = game:GetService("Lighting")
local bloom = Instance.new("BloomEffect")
bloom.Name = "Weather Bloom"
bloom.Enabled = true
bloom.Parent = Lighting
local depthOfField = Instance.new("DepthOfFieldEffect")
depthOfField.Name = "Weather Depth Of Field"
depthOfField.Enabled = true
depthOfField.Parent = Lighting
local godRays = Instance.new("GodRaysEffect")
godRays.Name = "Weather God Rays"
godRays.Enabled = true
godRays.Parent = Lighting
local Players = game:GetService("Players")
local RunService = game:GetService("RunService")
local StarterGui = game:GetService("StarterGui")
local Workspace = game:GetService("Workspace")

local MAXIMUM_FRAME_SECONDS = 1 / 20

type Weather = {
	Name: string,
	CloudShader: "Cumulus" | "Stratus" | "Storm" | "Voxel",
	Brightness: number,
	ClockTime: number,
	Ambient: Color3,
	OutdoorAmbient: Color3,
	BloomIntensity: number,
	DepthOfFieldIntensity: number,
	GodRayIntensity: number,
	CloudCover: number,
	CloudDensity: number,
	WeatherWindMetresPerSecond: number,
	BaseHeightMetres: number,
	ThicknessMetres: number,
	BaseTemperatureCelsius: number,
	TemperatureLapseCelsiusPerKilometre: number,
	TemperatureVariationCelsius: number,
	VapourMixingRatio: number,
	CondensateScale: number,
	FeatureCells: number,
	TurbulenceMetresPerSecond: number,
	ShearMetresPerSecond: number,
	LiftMetresPerSecond: number,
	FrontContrast: number,
	GrowthRate: number,
	AtmosphereDensity: number,
	AtmosphereHaze: number,
	AtmosphereGlare: number,
	FogDensity: number,
	FogExtinction: number,
	WeatherCoverage: number,
	FogStrength: number,
	PrecipitationRate: number,
	IceFraction: number,
	StormIntensity: number,
	HailIntensity: number,
	LightningRate: number,
	AtmosphereDensityStrength: number,
	AtmosphereHazeStrength: number,
	AtmosphereGlareSuppression: number,
}

local presets: { Weather } = {
	{
		Name = "CLEAR",
		CloudShader = "Cumulus",
		Brightness = 3.0,
		ClockTime = 16.2,
		Ambient = Color3.fromRGB(44, 51, 68),
		OutdoorAmbient = Color3.fromRGB(110, 132, 170),
		BloomIntensity = 0.72,
		DepthOfFieldIntensity = 0.32,
		GodRayIntensity = 0.42,
		CloudCover = 0.22,
		CloudDensity = 0.35,
		WeatherWindMetresPerSecond = 4,
		BaseHeightMetres = 768,
		ThicknessMetres = 384,
		BaseTemperatureCelsius = 20,
		TemperatureLapseCelsiusPerKilometre = 6.5,
		TemperatureVariationCelsius = 3,
		VapourMixingRatio = 0.005,
		CondensateScale = 0.001,
		FeatureCells = 8,
		TurbulenceMetresPerSecond = 0.8,
		ShearMetresPerSecond = 0.5,
		LiftMetresPerSecond = 0.1,
		FrontContrast = 0.12,
		GrowthRate = 0.08,
		AtmosphereDensity = 0.16,
		AtmosphereHaze = 0.55,
		AtmosphereGlare = 0.52,
		FogDensity = 0.05,
		FogExtinction = 0.2,
		WeatherCoverage = 0.34,
		FogStrength = 0.16,
		PrecipitationRate = 0,
		IceFraction = 0,
		StormIntensity = 0,
		HailIntensity = 0,
		LightningRate = 0,
		AtmosphereDensityStrength = 0.18,
		AtmosphereHazeStrength = 0.22,
		AtmosphereGlareSuppression = 0.1,
	},
	{
		Name = "SCATTERED CUMULUS",
		CloudShader = "Cumulus",
		Brightness = 2.6,
		ClockTime = 14.4,
		Ambient = Color3.fromRGB(42, 50, 65),
		OutdoorAmbient = Color3.fromRGB(106, 127, 158),
		BloomIntensity = 0.78,
		DepthOfFieldIntensity = 0.34,
		GodRayIntensity = 0.46,
		CloudCover = 0.54,
		CloudDensity = 0.48,
		WeatherWindMetresPerSecond = 6,
		BaseHeightMetres = 960,
		ThicknessMetres = 640,
		BaseTemperatureCelsius = 16,
		TemperatureLapseCelsiusPerKilometre = 6.5,
		TemperatureVariationCelsius = 7,
		VapourMixingRatio = 0.016,
		CondensateScale = 0.002,
		FeatureCells = 16,
		TurbulenceMetresPerSecond = 2,
		ShearMetresPerSecond = 3,
		LiftMetresPerSecond = 1.8,
		FrontContrast = 0.35,
		GrowthRate = 0.25,
		AtmosphereDensity = 0.18,
		AtmosphereHaze = 0.75,
		AtmosphereGlare = 0.42,
		FogDensity = 0.06,
		FogExtinction = 0.22,
		WeatherCoverage = 0.46,
		FogStrength = 0.18,
		PrecipitationRate = 0,
		IceFraction = 0,
		StormIntensity = 0,
		HailIntensity = 0,
		LightningRate = 0,
		AtmosphereDensityStrength = 0.22,
		AtmosphereHazeStrength = 0.32,
		AtmosphereGlareSuppression = 0.12,
	},
	{
		Name = "OVERCAST STRATUS",
		CloudShader = "Stratus",
		Brightness = 1.35,
		ClockTime = 14.4,
		Ambient = Color3.fromRGB(24, 30, 42),
		OutdoorAmbient = Color3.fromRGB(57, 73, 104),
		BloomIntensity = 1.0,
		DepthOfFieldIntensity = 0.54,
		GodRayIntensity = 0.68,
		CloudCover = 0.74,
		CloudDensity = 0.76,
		WeatherWindMetresPerSecond = 13,
		BaseHeightMetres = 1024,
		ThicknessMetres = 256,
		BaseTemperatureCelsius = 15,
		TemperatureLapseCelsiusPerKilometre = 6.5,
		TemperatureVariationCelsius = 8,
		VapourMixingRatio = 0.015,
		CondensateScale = 0.003,
		FeatureCells = 32,
		TurbulenceMetresPerSecond = 3,
		ShearMetresPerSecond = 4,
		LiftMetresPerSecond = 2,
		FrontContrast = 0.3,
		GrowthRate = 0.25,
		AtmosphereDensity = 0.34,
		AtmosphereHaze = 1.8,
		AtmosphereGlare = 0.12,
		FogDensity = 0.19,
		FogExtinction = 0.55,
		WeatherCoverage = 0.72,
		FogStrength = 0.48,
		PrecipitationRate = 38,
		IceFraction = 0.04,
		StormIntensity = 0.24,
		HailIntensity = 0.02,
		LightningRate = 0.25,
		AtmosphereDensityStrength = 0.42,
		AtmosphereHazeStrength = 0.55,
		AtmosphereGlareSuppression = 0.38,
	},
	{
		Name = "TOWERING STORM",
		CloudShader = "Storm",
		Brightness = 0.55,
		ClockTime = 19.3,
		Ambient = Color3.fromRGB(10, 14, 25),
		OutdoorAmbient = Color3.fromRGB(25, 37, 64),
		BloomIntensity = 1.8,
		DepthOfFieldIntensity = 0.72,
		GodRayIntensity = 0.9,
		CloudCover = 0.96,
		CloudDensity = 0.96,
		WeatherWindMetresPerSecond = 24,
		BaseHeightMetres = 896,
		ThicknessMetres = 1024,
		BaseTemperatureCelsius = 13,
		TemperatureLapseCelsiusPerKilometre = 6.5,
		TemperatureVariationCelsius = 10,
		VapourMixingRatio = 0.016,
		CondensateScale = 0.006,
		FeatureCells = 32,
		TurbulenceMetresPerSecond = 7,
		ShearMetresPerSecond = 10,
		LiftMetresPerSecond = 5,
		FrontContrast = 0.72,
		GrowthRate = 0.8,
		AtmosphereDensity = 0.52,
		AtmosphereHaze = 3.4,
		AtmosphereGlare = 0.02,
		FogDensity = 0.37,
		FogExtinction = 0.9,
		WeatherCoverage = 0.96,
		FogStrength = 0.82,
		PrecipitationRate = 150,
		IceFraction = 0.24,
		StormIntensity = 0.88,
		HailIntensity = 0.3,
		LightningRate = 1.4,
		AtmosphereDensityStrength = 0.68,
		AtmosphereHazeStrength = 0.82,
		AtmosphereGlareSuppression = 0.72,
	},
	{
		Name = "WISPY ICE",
		CloudShader = "Stratus",
		Brightness = 2.4,
		ClockTime = 10.8,
		Ambient = Color3.fromRGB(54, 67, 88),
		OutdoorAmbient = Color3.fromRGB(145, 170, 202),
		BloomIntensity = 0.9,
		DepthOfFieldIntensity = 0.24,
		GodRayIntensity = 0.5,
		CloudCover = 0.38,
		CloudDensity = 0.24,
		WeatherWindMetresPerSecond = 10,
		BaseHeightMetres = 2304,
		ThicknessMetres = 512,
		BaseTemperatureCelsius = -28,
		TemperatureLapseCelsiusPerKilometre = 6.5,
		TemperatureVariationCelsius = 3,
		VapourMixingRatio = 0.004,
		CondensateScale = 0.001,
		FeatureCells = 16,
		TurbulenceMetresPerSecond = 1,
		ShearMetresPerSecond = 4,
		LiftMetresPerSecond = 0.2,
		FrontContrast = 0.18,
		GrowthRate = 0.06,
		AtmosphereDensity = 0.18,
		AtmosphereHaze = 0.65,
		AtmosphereGlare = 0.6,
		FogDensity = 0.04,
		FogExtinction = 0.18,
		WeatherCoverage = 0.4,
		FogStrength = 0.12,
		PrecipitationRate = 8,
		IceFraction = 0.96,
		StormIntensity = 0.06,
		HailIntensity = 0,
		LightningRate = 0,
		AtmosphereDensityStrength = 0.2,
		AtmosphereHazeStrength = 0.28,
		AtmosphereGlareSuppression = 0.08,
	},
	{
		Name = "GROUND FOG",
		CloudShader = "Stratus",
		Brightness = 2.0,
		ClockTime = 7.2,
		Ambient = Color3.fromRGB(47, 56, 68),
		OutdoorAmbient = Color3.fromRGB(111, 128, 148),
		BloomIntensity = 0.45,
		DepthOfFieldIntensity = 0.62,
		GodRayIntensity = 0.2,
		CloudCover = 0.12,
		CloudDensity = 0.1,
		WeatherWindMetresPerSecond = 1.5,
		BaseHeightMetres = 128,
		ThicknessMetres = 256,
		BaseTemperatureCelsius = 17,
		TemperatureLapseCelsiusPerKilometre = 6.5,
		TemperatureVariationCelsius = 2,
		VapourMixingRatio = 0.014,
		CondensateScale = 0.001,
		FeatureCells = 8,
		TurbulenceMetresPerSecond = 0.3,
		ShearMetresPerSecond = 0.2,
		LiftMetresPerSecond = 0,
		FrontContrast = 0.08,
		GrowthRate = 0.02,
		AtmosphereDensity = 0.28,
		AtmosphereHaze = 2.8,
		AtmosphereGlare = 0.08,
		FogDensity = 0.3,
		FogExtinction = 0.85,
		WeatherCoverage = 0.14,
		FogStrength = 0.88,
		PrecipitationRate = 0,
		IceFraction = 0,
		StormIntensity = 0,
		HailIntensity = 0,
		LightningRate = 0,
		AtmosphereDensityStrength = 0.32,
		AtmosphereHazeStrength = 0.72,
		AtmosphereGlareSuppression = 0.45,
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

bloom.Threshold = 0.8
bloom.Radius = 8
bloom.Intensity = 1.0
depthOfField.FocusDistance = 54
depthOfField.FocusRange = 14
depthOfField.Radius = 7
depthOfField.Intensity = 0.54
godRays.Threshold = 0.72
godRays.Radius = 72
godRays.Intensity = 0.68

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

local weather = Instance.new("WeatherCompute")
weather.Name = "Weather Clouds"
weather.Shader = Enum.CloudComputeShader.Storm
weather.Color = Color3.fromRGB(83, 94, 112)
weather.WindDirection = Vector2.new(0.9, 0.3)
weather.CellSize = 0.24
weather.Detail = 0.82
weather.Height = 0.48
weather.Thickness = 0.68
weather.Steps = 32
weather.Seed = 1337
weather.LayerName = "weather-demo"
weather.Mode = Enum.WeatherMode.Procedural
weather.CellSizeMetres = 64
weather.BaseHeightMetres = 1024
weather.ThicknessMetres = 256
weather.ExtentMetres = Vector3.new(2048, 1024, 2048)
weather.FeatureCells = 32
weather.TemperatureLapseCelsiusPerKilometre = 6.5
weather.TickRate = 20
weather.MaximumResidentBricks = 512
weather.MaximumLod = 4
weather.ShadowsEnabled = true
weather.ShadowStrength = 0.72
weather.ShadowDistanceMetres = 2048
weather.ShadowResolution = 256
weather.ShadowCascades = 2
weather.Coverage = 0.72
weather.FogStrength = 0.48
weather.PrecipitationRate = 38
weather.IceFraction = 0.04
weather.StormIntensity = 0.24
weather.HailIntensity = 0.02
weather.LightningRate = 0.25
weather.AtmosphereDensityStrength = 0.42
weather.AtmosphereHazeStrength = 0.55
weather.AtmosphereGlareSuppression = 0.38
weather.Parent = Lighting

makePart("Weather Ground", Vector3.new(150, 1, 150), Vector3.new(0, -0.5, -28), Color3.fromRGB(26, 31, 38))
makePart("Weather Valley East Ridge", Vector3.new(10, 28, 112), Vector3.new(62, 13.5, -38), Color3.fromRGB(38, 44, 53))
makePart("Weather Valley West Ridge", Vector3.new(10, 28, 112), Vector3.new(-62, 13.5, -38), Color3.fromRGB(38, 44, 53))
makePart("Weather Precipitation Roof", Vector3.new(30, 2, 30), Vector3.new(30, 18, -32), Color3.fromRGB(55, 60, 69))
makePart("Weather Roof Receiver", Vector3.new(26, 0.12, 26), Vector3.new(30, 0.06, -32), Color3.fromRGB(72, 78, 84))
makePart("Weather Ruin Left", Vector3.new(12, 18, 8), Vector3.new(-22, 9, -38), Color3.fromRGB(53, 57, 63))
makePart("Weather Ruin Right", Vector3.new(9, 26, 9), Vector3.new(22, 13, -56), Color3.fromRGB(47, 52, 61))
local beacon = makePart("Weather Beacon", Vector3.new(4, 20, 4), Vector3.new(0, 10, -72), Color3.fromRGB(36, 42, 51))

for index = -3, 3 do
	local height = 5 + math.abs(index % 3) * 4
	makePart(
		"Weather Broken Column",
		Vector3.new(2.4, height, 2.4),
		Vector3.new(index * 9, height * 0.5, -50 - math.abs(index % 2) * 12),
		Color3.fromRGB(57, 64, 75)
	)
end

local beaconLight = Instance.new("PointLight")
beaconLight.Name = "Weather Beacon Light"
beaconLight.Color = Color3.fromRGB(127, 171, 255)
beaconLight.Brightness = 9
beaconLight.Range = 48
beaconLight.Parent = beacon

-- `Bounds` is a half-extent. The Y half-extent is intentionally small, making
-- this a squashed ellipsoid of mist along the ground instead of world fog.
local lowBank = Instance.new("Volume")
lowBank.Name = "Fog Field Low Bank"
lowBank.Position = Vector3.new(0, 4, -42)
lowBank.Bounds = Vector3.new(54, 4, 38)
lowBank.Shape = Enum.VolumeShape.Ellipsoid
lowBank.Color = Color3.fromRGB(157, 178, 201)
lowBank.Falloff = 0.68
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

local starterPlayerScripts = game:GetService('StarterPlayer'):FindFirstChild('StarterPlayerScripts')
assert(starterPlayerScripts ~= nil, 'StarterPlayerScripts is missing')
local demoCamera = starterPlayerScripts:FindFirstChild('DemoCamera')
assert(demoCamera ~= nil, 'DemoCamera is missing')

local sceneViews: { [string]: CFrame } = {
	GROUND = CFrame.lookAt(Vector3.new(43, 18, 42), Vector3.new(0, 46, -42)),
	VALLEY = CFrame.lookAt(Vector3.new(0, 12, 44), Vector3.new(0, 4, -42)),
	ROOF = CFrame.lookAt(Vector3.new(30, 9, -32), Vector3.new(30, 1, -32)),
	STORM = CFrame.lookAt(Vector3.new(43, 18, 42), Vector3.new(0, 1100, -42)),
}

local function sceneView(name: string, preset: Weather): CFrame?
	local layerCentre = preset.BaseHeightMetres + preset.ThicknessMetres * 0.5
	if name == "CLOUDS" then
		return CFrame.lookAt(Vector3.new(0, layerCentre, 700), Vector3.new(0, layerCentre, 0))
	end
	if name == "ABOVE" then
		local layerTop = preset.BaseHeightMetres + preset.ThicknessMetres
		return CFrame.lookAt(Vector3.new(0, layerTop + 500, 700), Vector3.new(0, layerCentre, 0))
	end
	return sceneViews[name]
end

demoCamera:SetAttribute('Name', 'Weather Camera')
demoCamera:SetAttribute('FieldOfView', 62)
demoCamera:SetAttribute('FarPlaneZ', 6000)
demoCamera:SetAttribute('CFrame', sceneViews.GROUND)

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
panel.Size = UDim2.new(0, 258, 0, 402)
panel.Parent = screen

local status = Instance.new("TextLabel")
status.Name = "Weather Status"
status.BackgroundTransparency = 1
status.Position = UDim2.new(0, 12, 0, 10)
status.Size = UDim2.new(1, -24, 0, 64)
status.TextColor3 = Color3.new(1, 1, 1)
status.TextSize = 13
status.TextWrapped = true
status.TextXAlignment = Enum.TextXAlignment.Left
status.TextYAlignment = Enum.TextYAlignment.Top
status.Parent = panel

local activePreset = 3
local target = presets[activePreset]
local fogMode = "LOW"
local fogSize = 1
local fogSquash = 1
local fogFluff = 1
local viewName = "GROUND"
local weatherMode = "PROCEDURAL"
local weatherPaused = false

local function applyWeather(preset: Weather)
	weather.WindMetresPerSecond = Vector3.new(0.9, 0, 0.3).Unit * preset.WeatherWindMetresPerSecond
	if preset.CloudShader == "Storm" then
		weather.Shader = Enum.CloudComputeShader.Storm
	elseif preset.CloudShader == "Stratus" then
		weather.Shader = Enum.CloudComputeShader.Stratus
	elseif preset.CloudShader == "Voxel" then
		weather.Shader = Enum.CloudComputeShader.Voxel
	else
		weather.Shader = Enum.CloudComputeShader.Cumulus
	end
	weather.BaseHeightMetres = preset.BaseHeightMetres
	weather.ThicknessMetres = preset.ThicknessMetres
	weather.BaseTemperatureCelsius = preset.BaseTemperatureCelsius
	weather.TemperatureLapseCelsiusPerKilometre = preset.TemperatureLapseCelsiusPerKilometre
	weather.TemperatureVariationCelsius = preset.TemperatureVariationCelsius
	weather.VapourMixingRatio = preset.VapourMixingRatio
	weather.CondensateScale = preset.CondensateScale
	weather.FeatureCells = preset.FeatureCells
	weather.TurbulenceMetresPerSecond = preset.TurbulenceMetresPerSecond
	weather.ShearMetresPerSecond = preset.ShearMetresPerSecond
	weather.LiftMetresPerSecond = preset.LiftMetresPerSecond
	weather.FrontContrast = preset.FrontContrast
	weather.GrowthRate = preset.GrowthRate
	weather.Coverage = preset.WeatherCoverage
	weather.FogStrength = preset.FogStrength
	weather.PrecipitationRate = preset.PrecipitationRate
	weather.IceFraction = preset.IceFraction
	weather.StormIntensity = preset.StormIntensity
	weather.HailIntensity = preset.HailIntensity
	weather.LightningRate = preset.LightningRate
	weather.AtmosphereDensityStrength = preset.AtmosphereDensityStrength
	weather.AtmosphereHazeStrength = preset.AtmosphereHazeStrength
	weather.AtmosphereGlareSuppression = preset.AtmosphereGlareSuppression
end

applyWeather(target)

local function applyFogProfile()
	local sizeScale = ({ 0.68, 1, 1.36 })[fogSize]
	local squashScale = ({ 0.45, 1, 1.75 })[fogSquash]
	local fluffiness = ({ 0.28, 0.74, 1 })[fogFluff]
	lowBank.Bounds = Vector3.new(54 * sizeScale, 4 * squashScale, 38 * sizeScale)
	lowBank.NoiseStrength = fluffiness
	lowBank.NoiseScale = if fogFluff == 3 then 0.11 else 0.16
end

local function updateStatus()
	status.Text = string.format(
		"WEATHER %s | %s\nVIEW %s | %s\nPRECIP %.0fmm/h CLOUD %.0f%%\nFOG %s | WIND %.0fm/s",
		target.Name,
		weatherMode,
		viewName,
		if weatherPaused then "PAUSED" else "RUNNING",
		target.PrecipitationRate,
		target.CloudCover * 100,
		fogMode,
		target.WeatherWindMetresPerSecond
	)
end

local function button(name: string, text: string, column: number, row: number): TextButton
	local control = Instance.new("TextButton")
	control.Name = name
	control.BackgroundColor3 = Color3.fromRGB(25, 35, 50)
	control.BorderSizePixel = 0
	control.Position = UDim2.new(0, 12 + column * 78, 0, 78 + row * 32)
	control.Size = UDim2.new(0, 70, 0, 28)
	control.Text = text
	control.TextColor3 = Color3.new(1, 1, 1)
	control.TextSize = 11
	control.Parent = panel
	return control
end

local function setView(name: string)
	local frame = sceneView(name, target)
	if frame == nil then
		return
	end
	viewName = name
	demoCamera:SetAttribute("CFrame", frame)
	updateStatus()
end

local presetLabels: { string } = { "CLEAR", "CUMULUS", "STRATUS", "STORM", "ICE", "FOG" }
for index, preset in presets do
	local column = (index - 1) % 3
	local row = math.floor((index - 1) / 3)
	local control = button("Weather Preset " .. preset.Name, presetLabels[index], column, row)
	control.Activated:Connect(function()
		activePreset = index
		target = presets[activePreset]
		applyWeather(target)
		if viewName == "CLOUDS" or viewName == "ABOVE" then
			setView(viewName)
		else
			updateStatus()
		end
	end)
end

button("Weather Scene Ground", "GROUND", 0, 2).Activated:Connect(function()
	setView("GROUND")
end)
button("Weather Scene Inside Clouds", "IN CLOUD", 1, 2).Activated:Connect(function()
	setView("CLOUDS")
end)
button("Weather Scene Above Clouds", "ABOVE", 2, 2).Activated:Connect(function()
	setView("ABOVE")
end)

button("Weather Scene Valley", "VALLEY", 0, 3).Activated:Connect(function()
	setView("VALLEY")
end)
button("Weather Scene Roof", "ROOF RAIN", 1, 3).Activated:Connect(function()
	activePreset = 3
	target = presets[activePreset]
	applyWeather(target)
	setView("ROOF")
end)
button("Weather Scene Storm", "STORM", 2, 3).Activated:Connect(function()
	activePreset = 4
	target = presets[activePreset]
	applyWeather(target)
	setView("STORM")
end)

local lowBankControl = button("Fog Low Bank", "LOW BANK", 0, 4)
lowBankControl.Activated:Connect(function()
	fogMode = "LOW"
	lowBank.Shape = Enum.VolumeShape.Ellipsoid
	fogSize = 1
	fogSquash = 1
	fogFluff = 2
	applyFogProfile()
	updateStatus()
end)

local squashedControl = button("Fog Squashed", "SQUASHED", 1, 4)
squashedControl.Activated:Connect(function()
	fogMode = "SQUASH"
	lowBank.Shape = Enum.VolumeShape.Ellipsoid
	fogSize = 3
	fogSquash = 1
	fogFluff = 1
	applyFogProfile()
	updateStatus()
end)

local fluffyControl = button("Fog Fluffy", "FLUFFY", 2, 4)
fluffyControl.Activated:Connect(function()
	fogMode = "FLUFF"
	fogFluff = 3
	applyFogProfile()
	updateStatus()
end)

local function slider(name: string, label: string, row: number): TextButton
	local track = Instance.new("TextButton")
	track.Name = name
	track.BackgroundColor3 = Color3.fromRGB(25, 35, 50)
	track.BorderSizePixel = 0
	track.Position = UDim2.new(0, 12, 0, 246 + row * 24)
	track.Size = UDim2.new(0, 234, 0, 22)
	track.Text = label
	track.TextColor3 = Color3.new(1, 1, 1)
	track.TextSize = 11
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

local modeControl = button("Weather Stateful Checkpoint", "PROCEDURAL", 0, 8)
modeControl.Position = UDim2.new(0, 12, 0, 362)
modeControl.Size = UDim2.new(0, 108, 0, 28)
modeControl.Activated:Connect(function()
	if weatherMode == "STATEFUL" then
		weatherMode = "PROCEDURAL"
		weather.Mode = Enum.WeatherMode.Procedural
		modeControl.Text = "PROCEDURAL"
	else
		weatherMode = "STATEFUL"
		weather.Mode = Enum.WeatherMode.Stateful
		modeControl.Text = "STATEFUL"
	end
	updateStatus()
end)

local pauseControl = button("Weather Pause Solver", "PAUSE", 1, 8)
pauseControl.Position = UDim2.new(0, 128, 0, 362)
pauseControl.Size = UDim2.new(0, 118, 0, 28)
pauseControl.Activated:Connect(function()
	weatherPaused = not weatherPaused
	weather.Paused = weatherPaused
	pauseControl.Text = if weatherPaused then "RESUME" else "PAUSE"
	updateStatus()
end)

updateStatus()

RunService.Heartbeat:Connect(function(deltaTime: number)
	local stepSeconds = math.min(math.max(deltaTime, 0), MAXIMUM_FRAME_SECONDS)
	local alpha = math.min(stepSeconds * 0.7, 1)

	Lighting.Brightness = lerp(Lighting.Brightness, target.Brightness, alpha)
	Lighting.ClockTime = lerp(Lighting.ClockTime, target.ClockTime, alpha)
	Lighting.Ambient = lerpColour(Lighting.Ambient, target.Ambient, alpha)
	Lighting.OutdoorAmbient = lerpColour(Lighting.OutdoorAmbient, target.OutdoorAmbient, alpha)
	bloom.Intensity = lerp(bloom.Intensity, target.BloomIntensity, alpha)
	depthOfField.Intensity = lerp(
		depthOfField.Intensity,
		target.DepthOfFieldIntensity,
		alpha
	)
	godRays.Intensity = lerp(godRays.Intensity, target.GodRayIntensity, alpha)
	atmosphere.Density = lerp(atmosphere.Density, target.AtmosphereDensity, alpha)
	atmosphere.Haze = lerp(atmosphere.Haze, target.AtmosphereHaze, alpha)
	atmosphere.Glare = lerp(atmosphere.Glare, target.AtmosphereGlare, alpha)
	weather.Cover = lerp(weather.Cover, target.CloudCover, alpha)
	weather.Density = lerp(weather.Density, target.CloudDensity, alpha)
	lowBank.Density = lerp(lowBank.Density, target.FogDensity, alpha)
	ruinMist.Density = lerp(ruinMist.Density, target.FogDensity * 1.3, alpha)
	lowBank.Extinction = lerp(lowBank.Extinction, target.FogExtinction, alpha)
	ruinMist.Extinction = lerp(ruinMist.Extinction, target.FogExtinction * 1.15, alpha)

end)
