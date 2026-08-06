--!strict

-- The world's shape, as a function of two integers.
--
-- Nothing here stores a map. A 16384 x 16384 world of one-stud columns is 268
-- million of them, and no arrangement of Luau tables holds that -- so the height
-- at a column is recomputed whenever anybody asks, and the map exists only as
-- this function plus whatever `Edits` has been layered on top.
--
-- ## The layers, and what each one decides
--
-- Six fields, combined into one height. They are separate because each answers a
-- different question and each wants its own wavelength:
--
--   * **warp** bends the sample point before anything else reads it. Every noise
--     field is built on an axis-aligned lattice and at low frequencies that
--     shows -- coasts run north-south, ranges run east-west, and the world looks
--     drawn on graph paper. This is two extra samples and it is the single most
--     valuable thing in the file.
--   * **continent** decides land or sea, at 3000 studs.
--   * **erosion** decides whether a region is worn flat or still sharp, at 900.
--     It multiplies the ridges rather than adding to the height, so a plain
--     stays a plain instead of becoming a lower mountain.
--   * **ridges** are the ranges themselves, ridged multifractal at 620.
--   * **detail** is the only per-stud layer, at 55.
--   * **climate** is temperature and humidity, at 2200, and picks the material.
--
-- ## The coarse lattice
--
-- Every layer except detail is sampled every `BASE_LATTICE` studs and
-- interpolated. Resolving a 3000-stud field at one-stud spacing asks a field
-- that barely moves across a whole chunk to be evaluated a thousand times inside
-- its own period; measured on the equivalent code, it is 16.5 ms to build a
-- chunk against 4.0, and the resulting terrain differs by about 1% in how well
-- it merges. Four times the cost for something nobody can see.
--
-- The lattice is global and aligned to the origin, so two chunks meeting at an
-- edge interpolate between the same two lattice samples and agree exactly. That
-- is what keeps it an optimisation rather than a source of seams.

local Noise = require(script.Parent.Noise)

local HeightField = {}
HeightField.__index = HeightField

export type Settings = {
	seed: number,
	-- Studs of water level above y = 0.
	seaLevel: number,
	-- Nothing is drawn below this.
	bedrock: number,
	continentScale: number,
	erosionScale: number,
	ridgeScale: number,
	detailScale: number,
	warpScale: number,
	climateScale: number,
	warpStrength: number,
	continentRelief: number,
	mountainRelief: number,
	detailRelief: number,
	caveScale: number,
	caveThreshold: number,
	caveCeiling: number,
	-- Wavelength of the region field that decides where caves exist at all, and
	-- the value it has to clear. Raising the threshold makes cave country rarer
	-- and chunks cheaper; `caveCeiling = 0` turns the whole feature off.
	caveRegionScale: number,
	caveRegionThreshold: number,
	snowLine: number,
	snowLineSwing: number,
}

export type Field = typeof(setmetatable(
	{} :: {
		settings: Settings,
		_edits: any,
	},
	HeightField
))

-- Studs between samples of the low-frequency layers. Chunk sizes are multiples
-- of it, which is what makes the lattice chunk-aligned.
local BASE_LATTICE = 8

-- Ridged noise sits high: measured mean 0.61, above 0.35 nearly everywhere. Used
-- raw, every square stud of land is on a ridge, which is the same as none of it
-- being. Everything below this becomes flat ground and what is left is rescaled.
local RIDGE_FLOOR = 0.45

-- Bare alpine rock between the treeline and the snow, in studs.
local ROCK_BAND = 14

local DEFAULTS: Settings = {
	seed = 20260807,
	seaLevel = 34,
	bedrock = -8,
	continentScale = 3000,
	erosionScale = 900,
	ridgeScale = 620,
	detailScale = 55,
	warpScale = 700,
	climateScale = 2200,
	warpStrength = 190,
	continentRelief = 46,
	mountainRelief = 78,
	detailRelief = 4.5,
	caveScale = 42,
	caveThreshold = 0.62,
	caveCeiling = 4,
	caveRegionScale = 1400,
	caveRegionThreshold = 0.25,
	snowLine = 34,
	snowLineSwing = 30,
}

-- The materials a column can show. Plain data, not Roblox types: `TerrainCore`
-- has to run headlessly for its tests, and the runtime is the one place a colour
-- becomes a `Color3`.
local SURFACES = table.freeze({
	table.freeze({ id = "rock", color = table.freeze({ r = 0.44, g = 0.43, b = 0.42 }), material = "Slate" }),
	table.freeze({ id = "grass", color = table.freeze({ r = 0.34, g = 0.53, b = 0.26 }), material = "Grass" }),
	table.freeze({ id = "sand", color = table.freeze({ r = 0.84, g = 0.78, b = 0.58 }), material = "Sand" }),
	table.freeze({ id = "snow", color = table.freeze({ r = 0.93, g = 0.95, b = 0.97 }), material = "Snow" }),
	table.freeze({ id = "tundra", color = table.freeze({ r = 0.59, g = 0.64, b = 0.58 }), material = "Ground" }),
	table.freeze({ id = "scrub", color = table.freeze({ r = 0.57, g = 0.56, b = 0.36 }), material = "LeafyGrass" }),
	table.freeze({ id = "water", color = table.freeze({ r = 0.20, g = 0.41, b = 0.60 }), material = "Glass" }),
	-- Freshly exposed rock, which only an edit produces. A crater reads as one
	-- because its floor is a material the untouched world never shows.
	table.freeze({ id = "scorch", color = table.freeze({ r = 0.17, g = 0.15, b = 0.15 }), material = "Basalt" }),
})

local SURFACE_ROCK = 1
local SURFACE_GRASS = 2
local SURFACE_SAND = 3
local SURFACE_SNOW = 4
local SURFACE_TUNDRA = 5
local SURFACE_SCRUB = 6
local SURFACE_WATER = 7
local SURFACE_SCORCH = 8

function HeightField.new(settings: { [string]: any }?, edits: any?): Field
	local resolved: { [string]: any } = table.clone(DEFAULTS :: any)
	for key, value in (settings or {}) :: any do
		assert(DEFAULTS[key :: any] ~= nil, "unknown terrain setting '" .. tostring(key) .. "'")
		resolved[key] = value
	end

	return setmetatable({ settings = resolved :: any, _edits = edits }, HeightField) :: any
end

-- The low-frequency height at a lattice point, in studs, before edits.
function HeightField.baseSample(self: Field, x: number, z: number): number
	local settings = self.settings
	local seed = settings.seed

	local warpX = Noise.noise2(x / settings.warpScale, z / settings.warpScale, seed + 101) * settings.warpStrength
	local warpZ = Noise.noise2(
		x / settings.warpScale + 5.5,
		z / settings.warpScale - 3.25,
		seed + 211
	) * settings.warpStrength

	local wx = x + warpX
	local wz = z + warpZ

	local continent = Noise.fbm2(wx / settings.continentScale, wz / settings.continentScale, seed + 1, 4, 2)

	-- The curve, not the noise, is what makes a coastline. Raising
	-- continentalness to an odd power leaves the sign alone and pushes the middle
	-- of the range towards zero, so most of the map is either clearly ocean or
	-- clearly land and the shelf between them is narrow. The raw field gives a
	-- world that is nearly all beach.
	local shaped = continent * continent * continent * 0.55 + continent * 0.45

	-- The floor is not zero: `(1 - erosion)^2` alone averages 0.25, so an
	-- honest-looking multiplier quietly removes three quarters of the relief
	-- everywhere. A worn range is lower than a young one, not absent.
	local erosion = (Noise.fbm2(wx / settings.erosionScale, wz / settings.erosionScale, seed + 2, 3, 2) + 1) * 0.5
	local relief = 0.18 + (1 - erosion) * (1 - erosion) * 1.15

	local ridges = Noise.ridged2(wx / settings.ridgeScale, wz / settings.ridgeScale, seed + 3, 5)
	ridges = math.max(ridges - RIDGE_FLOOR, 0) / (1 - RIDGE_FLOOR)

	-- Mountains only above water, ramping in over the first sixth of the
	-- continentalness range. Without the gate the same ranges appear on the sea
	-- floor and the shallows fill with islands one stud across. Set too steeply
	-- the ramp never reaches 1 and the ranges never rise, because `shaped` is a
	-- cube and spends most of its life near zero.
	local land = math.clamp(shaped * 6, 0, 1)

	return settings.seaLevel
		+ shaped * settings.continentRelief
		+ ridges * relief * settings.mountainRelief * land
end

-- The interpolated low-frequency height at any column.
--
-- Linear rather than faded: a smoothstep would flatten the slope at every
-- lattice line and terrace the hillsides at an 8-stud pitch. Linear gives a
-- constant slope inside each cell, and the per-stud detail layer hides the
-- corners between them.
function HeightField.baseAt(self: Field, x: number, z: number): number
	local gx = math.floor(x / BASE_LATTICE)
	local gz = math.floor(z / BASE_LATTICE)

	local tx = x / BASE_LATTICE - gx
	local tz = z / BASE_LATTICE - gz

	local x0 = gx * BASE_LATTICE
	local z0 = gz * BASE_LATTICE
	local x1 = x0 + BASE_LATTICE
	local z1 = z0 + BASE_LATTICE

	local north = Noise.lerp(self:baseSample(x0, z0), self:baseSample(x1, z0), tx)
	local south = Noise.lerp(self:baseSample(x0, z1), self:baseSample(x1, z1), tx)
	return Noise.lerp(north, south, tz)
end

-- The high-frequency half, in studs. Sampled per column, because at a 55-stud
-- wavelength it genuinely changes from one stud to the next.
--
-- Two octaves, not three. The third has a 12-stud wavelength, which at one-stud
-- voxels is fine enough to change the integer height every few columns, so it
-- breaks the run merge as well as costing a sample. Measured on the equivalent
-- code: two octaves 89.7 parts per chunk and 4.03 ms, three 92.8 and 4.60, four
-- 99.6 and 5.30. What the extra octaves add is below the voxel size anyway.
function HeightField.detailAt(self: Field, x: number, z: number): number
	local settings = self.settings
	return Noise.fbm2(x / settings.detailScale, z / settings.detailScale, settings.seed + 4, 2, 2.1)
		* settings.detailRelief
end

-- How much of the detail layer applies, given the low-frequency height under it.
--
-- Damped to a third at the waterline. A continental shelf is nearly flat, so at
-- full amplitude the detail layer crosses sea level over and over across it --
-- and every crossing is a sandbar one or two studs wide standing out of the
-- water. Hundreds of them, none merging with anything, each one a part. Measured
-- on the equivalent code, damping took a chunk from 117 parts to 78.
function HeightField.detailDamping(self: Field, base: number): number
	return math.clamp(math.abs(base - self.settings.seaLevel) / 9, 0.3, 1)
end

-- The generated height at a column, in studs, before edits.
function HeightField.naturalAt(self: Field, x: number, z: number): number
	local base = self:baseAt(x, z)
	return base + self:detailAt(x, z) * self:detailDamping(base)
end

-- The height a column actually has, edits included.
function HeightField.heightAt(self: Field, x: number, z: number): number
	local height = self:naturalAt(x, z)
	if self._edits ~= nil then
		height += self._edits:deltaAt(x, z)
	end
	return height
end

-- The climate at a column: temperature and humidity, each in [-1, 1].
--
-- The two fields are offset from each other in the domain as well as in the
-- seed, so a hot region is not also reliably a wet one.
function HeightField.climateAt(self: Field, x: number, z: number): (number, number)
	local settings = self.settings
	local scale = settings.climateScale
	return Noise.fbm2(x / scale, z / scale, settings.seed + 6, 3, 2),
		Noise.fbm2(x / scale + 17.5, z / scale + 9.25, settings.seed + 7, 3, 2)
end

-- Whether a column is in cave country at all, in [-1, 1].
--
-- **This is a 2D gate in front of a 3D test, and it was worth 37% of the cost of
-- building a chunk.** Measured over 49 chunks: 6.7 ms with the 3D cave test run
-- on every column, 3.8 ms with caves off entirely, 4.2 ms with this gate. The 3D
-- field is eight hashes and a trilinear blend and it was being evaluated once
-- per one-stud column for a feature whose wavelength is 42 studs -- the same
-- over-resolving mistake the coarse lattice exists to avoid, made again in the
-- one place the lattice did not cover.
--
-- It was also worth 16% of the box count, at 76.5 per chunk before and 64.1
-- after: a pit one column wide breaks a merged run in both axes, so scattered
-- caves cost parts as well as time.
--
-- And it is a better world. Caves used to be spread uniformly, which put
-- isolated pits everywhere; now they cluster into country that has caves and
-- country that does not.
function HeightField.caveMaskAt(self: Field, x: number, z: number): number
	local settings = self.settings
	return Noise.fbm2(
		x / settings.caveRegionScale,
		z / settings.caveRegionScale,
		settings.seed + 8,
		2,
		2
	)
end

-- Whether a voxel is hollow.
--
-- Only ever asked about the top few voxels of a column, because that is the only
-- place the answer changes what is drawn: a cave under solid rock is invisible,
-- and paying eight hashes to confirm it is invisible is the cheapest thing to
-- cut.
function HeightField.isCave(self: Field, x: number, y: number, z: number): boolean
	local settings = self.settings
	if y < settings.bedrock + 4 then
		return false
	end
	-- Squashed vertically, so caves come out as passages rather than as
	-- spherical bubbles.
	local field = Noise.noise3(
		x / settings.caveScale,
		y / (settings.caveScale * 0.55),
		z / settings.caveScale,
		settings.seed + 5
	)
	return math.abs(field) > settings.caveThreshold
end

-- The height of the topmost solid voxel, caves included.
--
-- Steps down at most `caveCeiling` times. A column whose whole ceiling is hollow
-- keeps the last height rather than falling to bedrock -- an honest floor under
-- an overhang, rather than a shaft the streaming would have to draw.
--
-- The height is passed in rather than sampled: the chunk builder holds a cached
-- lattice and would otherwise pay for the expensive half twice. `mask` is the
-- interpolated `caveMaskAt` for this column, which the builder also has; passing
-- nil makes this sample it, which is right for a one-off caller and wrong in a
-- loop.
function HeightField.solidTopAt(self: Field, x: number, z: number, height: number, mask: number?): number
	local top = math.floor(height)
	local settings = self.settings

	local region = if mask ~= nil then mask else self:caveMaskAt(x, z)
	if region < settings.caveRegionThreshold then
		return top
	end

	for _ = 1, settings.caveCeiling do
		if not self:isCave(x, top, z) then
			return top
		end
		top -= 1
	end

	return top
end

-- Which material a column shows.
--
-- The snow line is an altitude, not a temperature. Subtracting a lapse rate from
-- the climate field and thresholding the result is the obvious way to write this
-- and it puts snow on 60% of the land, because the lapse term swamps a climate
-- field whose typical magnitude is 0.25. Snow reads as an altitude to anyone
-- looking at it, so it is one here, and the climate moves the line rather than
-- competing with it.
--
-- `dug` is how far an edit lowered this column. Anything meaningfully below its
-- natural height shows scorched rock, which is what makes a crater legible as a
-- crater rather than as a dent in a grass field.
function HeightField.surfaceAt(
	self: Field,
	top: number,
	slope: number,
	warmth: number,
	humidity: number,
	dug: number?
): number
	local settings = self.settings

	if (dug or 0) > 1.5 then
		return SURFACE_SCORCH
	end

	if top < settings.seaLevel then
		return SURFACE_WATER
	end

	local snowLine = settings.seaLevel + settings.snowLine + warmth * settings.snowLineSwing

	if top > snowLine then
		return SURFACE_SNOW
	end

	-- Bare rock wherever the ground is too steep to hold anything, and in the
	-- band under the snow. The slope test is what draws a cliff face as a cliff
	-- rather than as vertical grass.
	if slope >= 3 or top > snowLine - ROCK_BAND then
		return SURFACE_ROCK
	end

	if warmth < -0.45 then
		return SURFACE_TUNDRA
	end

	-- The shoreline: a few studs of sand above the waterline, everywhere.
	if top <= settings.seaLevel + 2 then
		return SURFACE_SAND
	end

	if humidity < -0.3 then
		return if warmth > 0.25 then SURFACE_SAND else SURFACE_SCRUB
	end

	return SURFACE_GRASS
end

function HeightField.surfaces(): { any }
	return SURFACES
end

function HeightField.edits(self: Field): any
	return self._edits
end

function HeightField.setEdits(self: Field, edits: any)
	self._edits = edits
end

HeightField.Defaults = DEFAULTS
HeightField.Surfaces = SURFACES
HeightField.BaseLattice = BASE_LATTICE
HeightField.SurfaceRock = SURFACE_ROCK
HeightField.SurfaceGrass = SURFACE_GRASS
HeightField.SurfaceSand = SURFACE_SAND
HeightField.SurfaceSnow = SURFACE_SNOW
HeightField.SurfaceTundra = SURFACE_TUNDRA
HeightField.SurfaceScrub = SURFACE_SCRUB
HeightField.SurfaceWater = SURFACE_WATER
HeightField.SurfaceScorch = SURFACE_SCORCH
HeightField.New = HeightField.new

return HeightField
