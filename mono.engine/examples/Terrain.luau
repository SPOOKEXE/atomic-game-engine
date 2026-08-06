--!strict
-- A 16384 x 16384 noise-generated voxel world streamed around a camera.
--
-- Terrain is generated on demand; only nearby chunks exist.
--
-- The map is a deterministic function of coordinates. Only exposed, nearby
-- terrain is emitted, and adjacent columns are merged into boxes.

local Lighting = game:GetService("Lighting")

-- --- the map ---------------------------------------------------------------

-- Blocks along each axis. The world is centred on the origin, so coordinates
-- run from -8192 to 8191.
local MAP_BLOCKS = 16384

-- One metre, and every voxel in the file is this. It is a constant rather than
-- a literal `1` because half of it is needed constantly — a box of odd size
-- sits on a half-integer centre, and getting that wrong shifts the whole world
-- by half a block in a way that only shows at chunk seams.
local BLOCK = 1

-- Where water sits, in blocks above y = 0. Everything below it that is not
-- solid is filled.
local SEA_LEVEL = 34

-- Lowest emitted terrain level.
local BEDROCK = -8

-- Maximum exposed cliff height.
local MAX_EXPOSED = 24

-- --- the noise library -----------------------------------------------------
--
-- Gradient (Perlin-style) noise, seeded, with **no permutation table**.
--
-- The textbook implementation hashes a lattice corner by indexing a shuffled
-- 256-entry table, which makes the field repeat every 256 units. On a map 64
-- times wider than that the repeat is not subtle — it is a visible grid of
-- identical mountains, and it is the first thing that goes wrong when a
-- tutorial's noise meets a real world size. Hashing the coordinates
-- arithmetically instead gives a field whose period is 2^32 in each axis, which
-- is 262144 times the map.
--
-- `bit32` is exact here, and that is worth stating rather than assuming.
-- Luau converts a double to an unsigned with `(unsigned)(long long)` — so every
-- intermediate below 2^53 survives with no rounding, and every one of these
-- stays below 2^48. That is the whole reason the multipliers are 16-bit: a
-- 32x32 multiply reaches 2^64 and would silently start losing low bits, which
-- is a hash that degrades rather than one that fails.

local band = bit32.band
local bxor = bit32.bxor
local rshift = bit32.rshift

local MASK = 0xFFFFFFFF

-- One 32-bit integer in, one out, avalanched.
--
-- Three rounds of xor-shift-multiply. Two is measurably worse at the low bits,
-- which is exactly where the gradient index is read from.
local function scramble(value: number): number
	local x = band(value, MASK)
	x = band(bxor(x, rshift(x, 16)) * 0x9E37, MASK)
	x = band(bxor(x, rshift(x, 15)) * 0x85EB, MASK)
	x = band(bxor(x, rshift(x, 16)) * 0xC2B2, MASK)
	return bxor(x, rshift(x, 16))
end

-- A lattice corner's hash. The coordinates are folded in with large odd
-- constants before scrambling, so `(x, z)` and `(z, x)` do not collide and a
-- diagonal does not correlate with itself.
local function hash2(x: number, z: number, seed: number): number
	return scramble(band(x * 374761393 + z * 668265263 + seed * 2246822519, MASK))
end

local function hash3(x: number, y: number, z: number, seed: number): number
	return scramble(band(x * 374761393 + y * 1274126177 + z * 668265263 + seed * 2246822519, MASK))
end

-- Quintic fade, `6t^5 - 15t^4 + 10t^3`.
local function fade(t: number): number
	return t * t * t * (t * (t * 6 - 15) + 10)
end

local function lerp(a: number, b: number, t: number): number
	return a + (b - a) * t
end

-- The gradient at a corner, dotted with the offset to the sample point.
--
-- Eight directions rather than the classic twelve-in-3D set, chosen by three
-- bits of the hash: the four diagonals and the four axes. The diagonals are
-- what stop the field from looking axis-aligned; the axes are what stop it from
-- looking like a quilt of 45-degree ridges.
local function gradient2(corner: number, dx: number, dz: number): number
	local choice = band(corner, 7)
	if choice == 0 then
		return dx + dz
	elseif choice == 1 then
		return dx - dz
	elseif choice == 2 then
		return dz - dx
	elseif choice == 3 then
		return -dx - dz
	elseif choice == 4 then
		return dx
	elseif choice == 5 then
		return -dx
	elseif choice == 6 then
		return dz
	end
	return -dz
end

-- 2D gradient noise, roughly in [-1, 1].
local function noise2(x: number, z: number, seed: number): number
	local x0 = math.floor(x)
	local z0 = math.floor(z)
	local dx = x - x0
	local dz = z - z0

	local u = fade(dx)
	local v = fade(dz)

	local n00 = gradient2(hash2(x0, z0, seed), dx, dz)
	local n10 = gradient2(hash2(x0 + 1, z0, seed), dx - 1, dz)
	local n01 = gradient2(hash2(x0, z0 + 1, seed), dx, dz - 1)
	local n11 = gradient2(hash2(x0 + 1, z0 + 1, seed), dx - 1, dz - 1)

	return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v)
end

-- One lattice corner of the 3D field, in [-1, 1]. The hash is 32 bits, so
-- dividing by 2^31 and subtracting one is the whole conversion.
--
-- **A file-level function rather than a closure inside `noise3`.** It captures
-- nothing, and a closure that captures nothing is still allocated on every call
-- to the function containing it — which here was once per column, per chunk,
-- for a value that could have been made once.
local function corner3(x: number, y: number, z: number, seed: number): number
	return hash3(x, y, z, seed) / 2147483648.0 - 1.0
end

-- 3D value noise, in [-1, 1]. Value rather than gradient because it is only
-- used to carve caves, where the cheaper field is indistinguishable and the
-- eight gradient dot products are not.
local function noise3(x: number, y: number, z: number, seed: number): number
	local x0 = math.floor(x)
	local y0 = math.floor(y)
	local z0 = math.floor(z)

	local u = fade(x - x0)
	local v = fade(y - y0)
	local w = fade(z - z0)

	local c000 = corner3(x0, y0, z0, seed)
	local c100 = corner3(x0 + 1, y0, z0, seed)
	local c010 = corner3(x0, y0 + 1, z0, seed)
	local c110 = corner3(x0 + 1, y0 + 1, z0, seed)
	local c001 = corner3(x0, y0, z0 + 1, seed)
	local c101 = corner3(x0 + 1, y0, z0 + 1, seed)
	local c011 = corner3(x0, y0 + 1, z0 + 1, seed)
	local c111 = corner3(x0 + 1, y0 + 1, z0 + 1, seed)

	local y00 = lerp(lerp(c000, c100, u), lerp(c001, c101, u), w)
	local y11 = lerp(lerp(c010, c110, u), lerp(c011, c111, u), w)
	return lerp(y00, y11, v)
end

-- Dividing fBm by its amplitude sum bounds it at ±1 and leaves it nowhere near
-- ±1, because the octaves would all have to peak at the same place with the
-- same sign. Measured over 28k samples of the whole map, three- and four-octave
-- fBm here reaches ±0.53 and averages 0.136 in magnitude.
--
-- **This is the bug that makes a terrain sample look flat**, and it is invisible
-- in the code: every relief constant below is written as if the field spanned
-- [-1, 1], so a `MOUNTAIN_RELIEF` of 78 blocks actually delivers 41 at the
-- extreme and about 10 typically. The gain is applied here, once, rather than
-- being absorbed into constants that would then all be lying by the same
-- undocumented factor.
local FBM_GAIN = 1.85

-- Fractal Brownian motion: octaves at doubling frequency and halving amplitude,
-- normalised so the result spans very nearly [-1, 1] whatever the octave count.
--
-- The seed is offset per octave. Without that, every octave samples the *same*
-- field at a different zoom, and a feature at one scale reappears at the next —
-- which reads as a hillside made of copies of itself.
local function fbm2(x: number, z: number, seed: number, octaves: number, lacunarity: number): number
	local total = 0
	local amplitude = 1
	local sum = 0
	local frequency = 1

	for octave = 1, octaves do
		total += noise2(x * frequency, z * frequency, seed + octave * 1013) * amplitude
		sum += amplitude
		amplitude *= 0.5
		frequency *= lacunarity
	end

	-- Clamped, because the gain is calibrated against the *observed* extreme
	-- and a sample somewhere in 268 million columns will exceed it.
	return math.clamp(total / sum * FBM_GAIN, -1, 1)
end

-- Ridged multifractal: `1 - |noise|`, squared, octave-weighted.
--
-- The absolute value folds the field at zero, and a fold is a crease — which is
-- what a mountain ridge is. Squaring sharpens the peaks and flattens the
-- valleys, so the result is ranges with saddles between them rather than the
-- rolling dunes plain fBm gives.
--
-- **Each octave is weighted by the one above it**, which is the "multifractal"
-- half: detail appears on ridges that already exist and not in the valleys, so
-- the low ground stays smooth instead of being uniformly rough.
local function ridged2(x: number, z: number, seed: number, octaves: number): number
	local total = 0
	local amplitude = 1
	local sum = 0
	local frequency = 1
	local weight = 1

	for octave = 1, octaves do
		local signal = 1 - math.abs(noise2(x * frequency, z * frequency, seed + octave * 7919))
		signal *= signal
		signal *= weight

		-- Clamped, because an unbounded weight compounds across octaves and the
		-- eighth one can dominate the first.
		weight = math.clamp(signal * 2, 0, 1)

		total += signal * amplitude
		sum += amplitude
		amplitude *= 0.5
		frequency *= 2
	end

	return total / sum
end

-- --- the layers ------------------------------------------------------------
--
-- Six noise fields, each answering one question, combined into one height.
-- Written out as named constants rather than inline literals because the whole
-- character of the world is in these numbers and tuning them is the point of
-- having the file.

local SEED = 20260807

-- Wavelengths in blocks. A continent is 3 km across, a mountain range 900 m, a
-- boulder field 55 m. The map is 16 km, so it holds about five continents.
local CONTINENT_SCALE = 3000
local EROSION_SCALE = 900
local RIDGE_SCALE = 620
local DETAIL_SCALE = 55
local WARP_SCALE = 700
local CLIMATE_SCALE = 2200

-- How far the domain warp displaces a sample, in blocks.
--
-- **This is the single most valuable line in the file.** Every noise field is
-- built on an axis-aligned lattice, and at low frequencies that shows: coasts
-- run north-south, ranges run east-west, and the world looks like it was drawn
-- on graph paper. Sampling at `(x + warp, z + warp)` where the warp is itself
-- noise bends all of it, and costs two extra samples.
local WARP_STRENGTH = 190

-- Vertical scale, in blocks, for each layer's contribution.
local CONTINENT_RELIEF = 46
local MOUNTAIN_RELIEF = 78
local DETAIL_RELIEF = 4.5

-- Where a cave may exist, and how much of the field counts as air. Raising the
-- threshold makes caves rarer and narrower.
local CAVE_SCALE = 42
local CAVE_THRESHOLD = 0.62
local CAVE_CEILING = 6

-- The material a column shows, as an index into `SURFACES` below. An integer
-- rather than the table itself, because the run merge compares it once per
-- column and comparing tables by identity is a pointer compare the merge does
-- not need to think about.
local SURFACE_ROCK = 1
local SURFACE_GRASS = 2
local SURFACE_SAND = 3
local SURFACE_SNOW = 4
local SURFACE_TUNDRA = 5
local SURFACE_SCRUB = 6
local SURFACE_WATER = 7

local SURFACES = {
	{ Colour = Color3.fromRGB(112, 110, 106), Material = Enum.Material.Slate },
	{ Colour = Color3.fromRGB(86, 134, 66), Material = Enum.Material.Grass },
	{ Colour = Color3.fromRGB(214, 200, 148), Material = Enum.Material.Sand },
	{ Colour = Color3.fromRGB(238, 242, 247), Material = Enum.Material.Concrete },
	{ Colour = Color3.fromRGB(150, 162, 148), Material = Enum.Material.Concrete },
	{ Colour = Color3.fromRGB(146, 142, 92), Material = Enum.Material.Grass },
	{ Colour = Color3.fromRGB(52, 104, 152), Material = Enum.Material.Ice },
}

-- The transparency water is drawn at. Solid ground uses zero and never sets the
-- property, so only the water parts pay for a blend.
local WATER_TRANSPARENCY = 0.35

-- Ridged noise sits high: measured mean 0.61, and it is above 0.35 nearly
-- everywhere. Used raw, every square metre of land is on a ridge, which is the
-- same as none of it being. Everything below the floor becomes flat ground and
-- what is left is rescaled, so a range is a range and the space between two of
-- them is a valley.
local RIDGE_FLOOR = 0.45

-- --- the coarse lattice ----------------------------------------------------
--
-- **The low-frequency layers are sampled every 8 blocks and interpolated, not
-- sampled per block.**
--
-- Continents, erosion and ridges have wavelengths of 600 to 3000 blocks.
-- Evaluating a fourteen-octave stack of them once per one-metre column asks a
-- field that barely moves across a whole chunk to be resolved at a thousandth of
-- its own period. It is the single most expensive mistake available in this
-- file: measured over 96 chunks, **16.5 ms to build one against 4.0 ms**.
--
-- And it is invisible in the result — the same measurement puts the part count
-- at 91.0 per chunk sampled per column against 89.7 interpolated, a difference
-- of 1.4% in a number that is directly sensitive to the terrain's shape. Four
-- times the cost for nothing anyone could see.
--
-- 8 divides the chunk size, so the lattice is global and chunk-aligned. Two
-- chunks meeting at an edge interpolate between the *same* two lattice samples
-- and agree exactly — which is what keeps this an optimisation rather than a
-- source of seams.
--
-- The interpolation is linear rather than faded. A smoothstep would flatten the
-- slope at every lattice line and terrace the hillsides at an 8-block pitch;
-- linear gives a constant slope inside each cell, and the detail layer below —
-- which *is* sampled per block — hides the corners between them.
local BASE_LATTICE = 8

-- The low-frequency height at a lattice point, in blocks.
--
-- **Pure**: two integers and the file's constants, nothing else. That is what
-- lets a chunk be rebuilt after being released and come back identical, and
-- what lets the neighbour lookups below sample outside the chunk without
-- needing the neighbouring chunk to exist.
local function BaseSample(x: number, z: number): number
	-- The warp first, at a wavelength between a continent and a range, so it
	-- bends coastlines and ridges alike.
	local warpX = noise2(x / WARP_SCALE, z / WARP_SCALE, SEED + 101) * WARP_STRENGTH
	local warpZ = noise2(x / WARP_SCALE + 5.5, z / WARP_SCALE - 3.25, SEED + 211) * WARP_STRENGTH

	local wx = x + warpX
	local wz = z + warpZ

	-- Continentalness, in [-1, 1]. Negative is sea floor, positive is inland.
	local continent = fbm2(wx / CONTINENT_SCALE, wz / CONTINENT_SCALE, SEED + 1, 4, 2)

	-- **The curve, not the noise, is what makes a coastline.** Raising
	-- continentalness to an odd power leaves the sign alone and pushes the
	-- middle of the range towards zero, so most of the map is either clearly
	-- ocean or clearly land and the shelf between them is narrow. Feeding the
	-- raw field in gives a world that is nearly all beach.
	local shaped = continent * continent * continent * 0.55 + continent * 0.45

	-- Erosion, in [0, 1]. High means old, worn, flat; low means young and
	-- mountainous. It multiplies the ridges rather than adding to the height,
	-- which is why a plain stays a plain instead of becoming a lower mountain.
	--
	-- The floor is not zero: `(1 - erosion)^2` alone averages 0.25, so an
	-- honest-looking multiplier quietly removes three quarters of the relief
	-- everywhere. A worn range is lower than a young one, not absent.
	local erosion = (fbm2(wx / EROSION_SCALE, wz / EROSION_SCALE, SEED + 2, 3, 2) + 1) * 0.5
	local relief = 0.18 + (1 - erosion) * (1 - erosion) * 1.15

	local ridges = ridged2(wx / RIDGE_SCALE, wz / RIDGE_SCALE, SEED + 3, 5)
	ridges = math.max(ridges - RIDGE_FLOOR, 0) / (1 - RIDGE_FLOOR)

	-- Mountains only above water, ramping in over the first fifth of the
	-- continentalness range. Without the gate the same ranges appear on the sea
	-- floor and the shallows fill with islands one block across; with it set too
	-- steeply — `shaped * 2.5` was the first attempt — the ramp never reaches 1
	-- and the ranges never rise, because `shaped` is a cube and spends most of
	-- its life near zero.
	local land = math.clamp(shaped * 6, 0, 1)

	return SEA_LEVEL + shaped * CONTINENT_RELIEF + ridges * relief * MOUNTAIN_RELIEF * land
end

-- The interpolated low-frequency height at any column.
local function BaseAt(x: number, z: number): number
	local gx = math.floor(x / BASE_LATTICE)
	local gz = math.floor(z / BASE_LATTICE)

	local tx = x / BASE_LATTICE - gx
	local tz = z / BASE_LATTICE - gz

	local x0 = gx * BASE_LATTICE
	local z0 = gz * BASE_LATTICE
	local x1 = x0 + BASE_LATTICE
	local z1 = z0 + BASE_LATTICE

	local north = lerp(BaseSample(x0, z0), BaseSample(x1, z0), tx)
	local south = lerp(BaseSample(x0, z1), BaseSample(x1, z1), tx)
	return lerp(north, south, tz)
end

-- The high-frequency half, in blocks. Sampled per column, because at a 55-metre
-- wavelength it genuinely changes from one block to the next — and it is
-- therefore the single hottest call in the file, run 1156 times per chunk.
--
-- **Two octaves, not three.** The third has a 12-metre wavelength, which at
-- one-metre voxels is fine enough to change the integer height every few
-- columns — so it breaks the run merge as well as costing a sample. Measured
-- over 96 chunks: two octaves 89.7 parts and 4.03 ms, three 92.8 and 4.60, four
-- 99.6 and 5.30. What the extra octaves add is below the voxel size anyway.
local function DetailAt(x: number, z: number): number
	return fbm2(x / DETAIL_SCALE, z / DETAIL_SCALE, SEED + 4, 2, 2.1) * DETAIL_RELIEF
end

-- How much of the detail layer applies, given the low-frequency height under it.
--
-- **Damped to a third at the waterline.** A continental shelf is nearly flat, so
-- at full amplitude the detail layer crosses sea level over and over across it —
-- and every crossing is a sandbar one or two blocks wide standing out of the
-- water. Hundreds of them, none merging with anything, and each one a part. The
-- ramp is nine blocks deep, which is wide enough that no terrace appears at the
-- boundary.
local function DetailDamping(base: number): number
	return math.clamp(math.abs(base - SEA_LEVEL) / 9, 0.3, 1)
end

-- The surface height at a column, in blocks, before caves.
--
-- Used by the camera and by nothing in the chunk builder, which reaches the two
-- halves separately so it can cache the expensive one. The two paths must stay
-- the same sum — a camera flying at a height the terrain does not have is the
-- symptom when they drift.
local function HeightAt(x: number, z: number): number
	local base = BaseAt(x, z)
	return base + DetailAt(x, z) * DetailDamping(base)
end

-- Whether a voxel is hollow.
--
-- Only ever asked about the top few voxels of a column, because that is the
-- only place the answer changes what is drawn: a cave under solid rock is
-- invisible, and paying eight hashes to confirm it is invisible is the cheapest
-- thing to cut.
local function IsCave(x: number, y: number, z: number): boolean
	if y < BEDROCK + 4 then
		return false
	end
	-- Squashed vertically, so caves come out as passages rather than as
	-- spherical bubbles.
	local field = noise3(x / CAVE_SCALE, y / (CAVE_SCALE * 0.55), z / CAVE_SCALE, SEED + 5)
	return math.abs(field) > CAVE_THRESHOLD
end

-- The height of the topmost solid voxel, caves included.
--
-- Steps down at most `CAVE_CEILING` times. A column whose whole ceiling is
-- hollow keeps the last height rather than falling to bedrock — an honest floor
-- under an overhang, rather than a shaft the streaming would have to draw.
--
-- **The height is passed in rather than sampled.** The caller has it already:
-- the chunk builder holds a cached lattice and would otherwise pay for the
-- expensive half twice, and hiding that sample inside here is what would make
-- the cache look useless.
local function SolidTopAt(x: number, z: number, height: number): number
	local top = math.floor(height)

	for _ = 1, CAVE_CEILING do
		if not IsCave(x, top, z) then
			return top
		end
		top -= 1
	end

	return top
end

-- The climate at a lattice point: temperature and humidity, each in [-1, 1].
--
-- On the same coarse lattice as the height, and for a stronger reason: at a
-- 2200-block wavelength a biome boundary moves by less than a millimetre from
-- one column to the next, so a per-column sample buys nothing at all. The two
-- fields are offset from each other in the domain as well as in the seed, so a
-- hot region is not also reliably a wet one.
local function ClimateSample(x: number, z: number): (number, number)
	return fbm2(x / CLIMATE_SCALE, z / CLIMATE_SCALE, SEED + 6, 3, 2),
		fbm2(x / CLIMATE_SCALE + 17.5, z / CLIMATE_SCALE + 9.25, SEED + 7, 3, 2)
end

-- How high snow lies in a temperate region, in blocks above sea level, and how
-- far the climate moves that line either way. A cold region's line falls to
-- sea level and a hot one's rises above the tallest peak the height field
-- produces, so the poles are white to the shore and the tropics are bare to the
-- summit.
local SNOW_LINE = 34
local SNOW_LINE_SWING = 30

-- Bare alpine rock between the treeline and the snow, in blocks.
local ROCK_BAND = 14

-- Which material a column shows, from its height, its slope and its climate.
--
-- **The snow line is an altitude, not a temperature.** Subtracting a lapse rate
-- from the climate field and thresholding the result is the obvious way to
-- write this and it was the first attempt: it puts snow on 60% of the land,
-- because the lapse term swamps a climate field whose typical magnitude is 0.25.
-- Snow reads as an altitude to anyone looking at it, so it is one here, and the
-- climate moves the line rather than competing with it.
local function SurfaceAt(top: number, slope: number, warmth: number, humidity: number): number
	if top < SEA_LEVEL then
		return SURFACE_WATER
	end

	local snowLine = SEA_LEVEL + SNOW_LINE + warmth * SNOW_LINE_SWING

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

	-- The shoreline: a few blocks of sand above the waterline, everywhere.
	if top <= SEA_LEVEL + 2 then
		return SURFACE_SAND
	end

	if humidity < -0.3 then
		return if warmth > 0.25 then SURFACE_SAND else SURFACE_SCRUB
	end

	return SURFACE_GRASS
end

-- --- the part pool ---------------------------------------------------------
--
-- **Parts are recycled, never destroyed.** A chunk leaving the view radius
-- hands its parts back; the next chunk to be built takes them. The alternative
-- churns thousands of instances a second through `CreateInstance` and
-- `Destroy`, and every one of those is a structural change to the store — the
-- archetype move `ecs/Store.hpp` defers, multiplied by the streaming rate.
--
-- `Visible = false` is what makes a pooled part free: `scene::Visual::Visible`
-- decides whether an entity is submitted at all, so an idle part costs a row
-- and no draw. Setting `Parent` to nothing would have been the Roblox habit and
-- is not available here — `Instance.Parent` is not optional in the bindings.

-- How many idle parts the pool may hold before the rest are destroyed.
--
-- **A high-water-mark pool is not free**, which is the half of this that is easy
-- to miss. How many parts a chunk needs depends on the terrain in it — measured
-- 29 over water and 260 in mountains, a factor of nine — so a camera that
-- crosses a range and then flies out over an ocean leaves the pool holding the
-- difference forever. Measured over a minute of the path below: 15500 instances
-- alive of which 9200 were idle, and an idle part is still an ECS row the
-- visibility walk visits.
--
-- The cap trades that for some churn, and only while the working set is
-- shrinking. Same minute with it: **7951 instances alive of which 1647 were
-- idle**, for 8811 destroys — 147 a second, against a streaming rate that is
-- already creating about that many.
local POOL_LIMIT = 2048

local pool: { Part } = {}
local poolCount = 0
local created = 0
local destroyed = 0

local function Acquire(): Part
	if poolCount > 0 then
		local part = pool[poolCount]
		pool[poolCount] = nil
		poolCount -= 1
		part.Visible = true
		return part
	end

	created += 1
	local part = Instance.new("Part")
	part.Name = "Voxels"
	part.Anchored = true
	part.Parent = workspace
	return part
end

local function Release(part: Part)
	if poolCount >= POOL_LIMIT then
		destroyed += 1
		part:Destroy()
		return
	end

	part.Visible = false
	poolCount += 1
	pool[poolCount] = part
end

-- --- chunks ----------------------------------------------------------------

-- Blocks along one edge of a chunk. The unit of building, releasing and
-- budgeting.
--
-- 32 is a compromise both ways. Smaller chunks stream more smoothly and merge
-- worse, because a run cannot cross a chunk edge; larger ones merge better and
-- land as a visible hitch. 32 keeps the longest possible run at 32 metres,
-- which is most of a flat row.
local CHUNK = 32

-- How far the built region reaches, in chunks.
--
-- **A circle, not a square, and it is worth a third of the world.** A square
-- region reaches 1.41 times further at its corners than along its axes — so
-- with the fog below set from the axis distance, every corner chunk is built,
-- meshed, and then hidden by fog. The 11x11 square is 121 chunks and the
-- inscribed circle is 81, for a view distance that is the same everywhere the
-- camera can look instead of only in four directions. Measured over a minute of
-- the path below, the change took the working set from 142 chunks to 96.
--
-- **This is the one number to turn**, and it is quadratic. At 5 the steady state
-- is 96 chunks, which over a minute of the path below held between 6300 and
-- 15400 drawn parts as the terrain changed under it, for 160 metres of view.
-- 6 is about 140 chunks and 192 metres, 4 about 60 and 128. One-metre voxels do
-- not buy a long view cheaply.
local VIEW_CHUNKS = 5

-- One chunk further than the load radius, so a camera hovering exactly on a
-- boundary does not build and release the same ring every beat. Hysteresis, and
-- the cheapest kind there is — at the cost of the steady-state working set
-- sitting between the two circles rather than on the inner one.
local RELEASE_CHUNKS = VIEW_CHUNKS + 1

-- Compared against squared distances throughout, so the streaming loop never
-- takes a square root to answer a question it can answer without one.
local VIEW_CHUNKS_SQUARED = VIEW_CHUNKS * VIEW_CHUNKS
local RELEASE_CHUNKS_SQUARED = RELEASE_CHUNKS * RELEASE_CHUNKS

-- How many chunks may be built in one beat, and how many before the first
-- frame.
--
-- **The whole cost of this file is here.** A chunk measures 4.4 ms of Luau, so
-- one per beat is a quarter of a 60 Hz tick and three would be most of it.
-- One is also far more than the camera needs: at the path speed below it enters
-- a new chunk every 1.4 seconds and needs eleven to fill the leading edge, which
-- is 8 a second against a budget of 60.
--
-- The prefill is a separate, larger number because it is spent once, at load,
-- against the top-level script's own step budget rather than a beat's. Nine
-- chunks is the ring immediately around the camera — enough that the first
-- frame has ground under it and `LoadScene` measures a world rather than a
-- camera.
local CHUNKS_PER_BEAT = 1
local PREFILL_CHUNKS = 9

type Chunk = {
	Parts: { Part },
	Count: number,
}

local chunks: { [string]: Chunk } = {}
local loadedChunks = 0

-- The key a chunk is filed under. A string because Luau tables have no tuple
-- key, and packing two coordinates into one number would need a range check
-- this does not otherwise need.
local function ChunkKey(cx: number, cz: number): string
	return `{cx}:{cz}`
end

-- Places one part as the axis-aligned box a merged group of voxels occupies.
--
-- Every bound is an **inclusive** block index, which is what the merge produces
-- and what makes the extents read as counts. The centre lands on a half-integer
-- whenever a span is odd, which is correct and is the arithmetic worth reading
-- twice: a single block at index 4 occupies [4, 5) and is centred at 4.5.
local function PlaceBox(
	target: { Part },
	count: number,
	x0: number,
	x1: number,
	z0: number,
	z1: number,
	bottom: number,
	top: number,
	surface: number
): number
	local width = (x1 - x0 + 1) * BLOCK
	local height = (top - bottom + 1) * BLOCK
	local depth = (z1 - z0 + 1) * BLOCK

	local part = Acquire()
	part.Size = Vector3.new(width, height, depth)
	part.Position = Vector3.new(
		(x0 * BLOCK) + width * 0.5,
		(bottom * BLOCK) + height * 0.5,
		(z0 * BLOCK) + depth * 0.5
	)

	local entry = SURFACES[surface]
	part.Color = entry.Colour
	part.Material = entry.Material
	part.Transparency = if surface == SURFACE_WATER then WATER_TRANSPARENCY else 0

	-- Water casts no shadow. A translucent shadow caster is the expensive kind
	-- and an ocean is the largest surface in the scene, so this is the one
	-- property here that is a frame-time decision rather than an appearance one.
	part.CastShadow = surface ~= SURFACE_WATER

	local at = count + 1
	target[at] = part
	return at
end

-- Builds one chunk: samples its columns, works out what of each is exposed, and
-- emits merged runs.
local function BuildChunk(cx: number, cz: number): Chunk
	local originX = cx * CHUNK
	local originZ = cz * CHUNK

	-- The lattice cell range the padded chunk falls in, plus one so the far side
	-- of the last cell has a corner to interpolate towards. Seven by seven for a
	-- 32-block chunk on an 8-block lattice — 49 samples of the expensive layers
	-- where a per-column evaluation would have taken 1156.
	local latticeX0 = (originX - 1) // BASE_LATTICE
	local latticeZ0 = (originZ - 1) // BASE_LATTICE
	local latticeSpan = (CHUNK + 1) // BASE_LATTICE + 2

	local base: { number } = {}
	local warmth: { number } = {}
	local damp: { number } = {}

	for gz = 0, latticeSpan do
		for gx = 0, latticeSpan do
			local at = gz * (latticeSpan + 1) + gx + 1
			local wx = (latticeX0 + gx) * BASE_LATTICE
			local wz = (latticeZ0 + gz) * BASE_LATTICE
			base[at] = BaseSample(wx, wz)
			warmth[at], damp[at] = ClimateSample(wx, wz)
		end
	end

	-- Bilinear lookup into the three lattice grids at once, so a column pays for
	-- one set of weights rather than three.
	local function LatticeAt(x: number, z: number): (number, number, number)
		local cellX = x / BASE_LATTICE - latticeX0
		local cellZ = z / BASE_LATTICE - latticeZ0

		local gx = math.floor(cellX)
		local gz = math.floor(cellZ)
		local tx = cellX - gx
		local tz = cellZ - gz

		local row = gz * (latticeSpan + 1) + gx + 1
		local next_ = row + latticeSpan + 1

		return lerp(lerp(base[row], base[row + 1], tx), lerp(base[next_], base[next_ + 1], tx), tz),
			lerp(lerp(warmth[row], warmth[row + 1], tx), lerp(warmth[next_], warmth[next_ + 1], tx), tz),
			lerp(lerp(damp[row], damp[row + 1], tx), lerp(damp[next_], damp[next_ + 1], tx), tz)
	end

	-- **A one-column border, sampled but never drawn.** A column's exposed
	-- height depends on its four neighbours, and the neighbours of the edge
	-- columns are in the next chunk. Sampling them here rather than asking the
	-- neighbouring chunk is what keeps chunks independent — they can be built in
	-- any order, and a chunk at the edge of the loaded region is not missing
	-- anything. It costs a 34x34 grid instead of 32x32, which is 13% more work
	-- for a seam that never appears.
	local tops: { number } = {}
	local warmths: { number } = {}
	local damps: { number } = {}

	for iz = -1, CHUNK do
		for ix = -1, CHUNK do
			local at = (iz + 1) * (CHUNK + 2) + (ix + 1) + 1
			local wx = originX + ix
			local wz = originZ + iz

			-- One bilinear lookup feeds both the height and the material, which
			-- is why they are stored together rather than the climate being
			-- fetched again in the emit loop below.
			local columnBase, columnWarmth, columnDamp = LatticeAt(wx, wz)

			-- The same sum `HeightAt` produces, spelled out because the base
			-- half came from the cache. The two must not drift.
			tops[at] = SolidTopAt(wx, wz, columnBase + DetailAt(wx, wz) * DetailDamping(columnBase))
			warmths[at] = columnWarmth
			damps[at] = columnDamp
		end
	end

	local function TopAt(ix: number, iz: number): number
		return tops[(iz + 1) * (CHUNK + 2) + (ix + 1) + 1]
	end

	local function WarmthAt(ix: number, iz: number): number
		return warmths[(iz + 1) * (CHUNK + 2) + (ix + 1) + 1]
	end

	local function DampAt(ix: number, iz: number): number
		return damps[(iz + 1) * (CHUNK + 2) + (ix + 1) + 1]
	end

	-- **The second merge axis.** A run closed on one row is not placed straight
	-- away: it is held open, and if the next row produces a run with the same
	-- span, top, bottom and material it is extended in Z instead. That is the
	-- rest of greedy meshing, and it is what collapses an ocean or a plain from
	-- one part per row to one part.
	--
	-- Held in one table keyed by the four fields that have to match, so the
	-- lookup is a hash rather than a scan of the previous row. `Z1` is the last
	-- row the box reaches, and `Row` is which row it was last extended on —
	-- without the second, a run that skipped a row would rejoin the box below it
	-- and swallow the gap.
	type Box = {
		X0: number,
		X1: number,
		Z0: number,
		Z1: number,
		Bottom: number,
		Top: number,
		Surface: number,
	}

	local boxes: { Box } = {}
	local boxCount = 0

	local open: { [string]: Box } = {}
	local openRow: { [string]: number } = {}

	local function EmitRun(x0: number, x1: number, iz: number, bottom: number, top: number, surface: number)
		local key = `{x0}:{x1}:{bottom}:{top}:{surface}`
		local grown = open[key]

		if grown and openRow[key] == iz - 1 then
			grown.Z1 = iz
			openRow[key] = iz
			return
		end

		local box: Box = {
			X0 = originX + x0,
			X1 = originX + x1,
			Z0 = iz,
			Z1 = iz,
			Bottom = bottom,
			Top = top,
			Surface = surface,
		}

		boxCount += 1
		boxes[boxCount] = box
		open[key] = box
		openRow[key] = iz
	end

	for iz = 0, CHUNK - 1 do
		-- The run being accumulated. `runStart` of -1 means there is none.
		local runStart = -1
		local runBottom = 0
		local runTop = 0
		local runSurface = 0

		local waterStart = -1

		for ix = 0, CHUNK do
			-- One column past the end, with no column of its own, so the loop
			-- body has exactly one place that flushes a run rather than a body
			-- and a copy of it after the loop.
			local inside = ix < CHUNK

			local top = 0
			local bottom = 0
			local surface = 0

			if inside then
				top = TopAt(ix, iz)

				-- The lowest of the four neighbours is where this column stops
				-- being visible. Everything below that is enclosed on all four
				-- sides and can never be seen from outside the hill.
				local lowest = math.min(
					TopAt(ix - 1, iz),
					TopAt(ix + 1, iz),
					TopAt(ix, iz - 1),
					TopAt(ix, iz + 1)
				)

				-- The slope, in blocks, which the material choice reads. It is
				-- free here and would cost four more height samples anywhere
				-- else.
				local slope = top - lowest

				bottom = math.max(math.min(lowest + 1, top), top - MAX_EXPOSED, BEDROCK)
				surface = SurfaceAt(top, slope, WarmthAt(ix, iz), DampAt(ix, iz))
			end

			-- Flush the solid run when the next column disagrees with it, or
			-- when there is no next column.
			if runStart >= 0 and (not inside or top ~= runTop or bottom ~= runBottom or surface ~= runSurface) then
				EmitRun(runStart, ix - 1, iz, runBottom, runTop, runSurface)
				runStart = -1
			end

			if inside and runStart < 0 then
				runStart = ix
				runTop = top
				runBottom = bottom
				runSurface = surface
			end

			-- Water is a second run over the same columns: **one block thick, at
			-- sea level**, rather than a solid box filled down to the sea floor.
			--
			-- A filled box is what the voxel field literally says is there, and
			-- it is the wrong thing to draw. Every water run would then have a
			-- different bottom — the sea floor under it — so none would merge in
			-- either axis, and half the map would cost a part per column. A
			-- surface has the same top and bottom everywhere, so a whole bay
			-- becomes one box. Nothing is lost as long as the camera stays above
			-- it, which `EYE_CLEARANCE` guarantees.
			local wet = inside and top < SEA_LEVEL

			if waterStart >= 0 and not wet then
				EmitRun(waterStart, ix - 1, iz, SEA_LEVEL, SEA_LEVEL, SURFACE_WATER)
				waterStart = -1
			end

			if wet and waterStart < 0 then
				waterStart = ix
			end
		end
	end

	local parts: { Part } = {}
	local count = 0

	for index = 1, boxCount do
		local box = boxes[index]
		count = PlaceBox(parts, count, box.X0, box.X1, originZ + box.Z0, originZ + box.Z1, box.Bottom, box.Top, box.Surface)
	end

	return { Parts = parts, Count = count }
end

local function ReleaseChunk(chunk: Chunk)
	for index = 1, chunk.Count do
		Release(chunk.Parts[index])
	end
end

-- --- streaming -------------------------------------------------------------

-- The chunk a world coordinate falls in. `math.floor` rather than truncation,
-- because the map is centred on the origin and half of it is negative:
-- truncation would make chunk 0 twice as wide as every other one, and the seam
-- would sit exactly where the camera starts.
local function ChunkOf(worldValue: number): number
	return math.floor(worldValue / (CHUNK * BLOCK))
end

local HALF_MAP = MAP_BLOCKS / 2
local MIN_CHUNK = -HALF_MAP // CHUNK
local MAX_CHUNK = HALF_MAP // CHUNK - 1

-- Builds what is missing near `(cx, cz)` and releases what is far, up to the
-- per-beat budget.
--
-- **Nearest first.** The candidate list is walked in rings out from the camera,
-- so a budget that only covers part of the work covers the part in front of the
-- viewer — which is the difference between terrain that appears at the horizon
-- and terrain that appears under you.
local function Stream(cx: number, cz: number, budget: number): number
	local built = 0

	for radius = 0, VIEW_CHUNKS do
		for dz = -radius, radius do
			for dx = -radius, radius do
				-- Only the perimeter of this ring; the interior was covered by
				-- a smaller radius.
				if radius > 0 and math.abs(dx) ~= radius and math.abs(dz) ~= radius then
					continue
				end

				-- The rings are square and the region is round, so the corners
				-- of each ring fall outside it. Walking square rings and
				-- rejecting the corners keeps "nearest first" without sorting
				-- anything.
				if dx * dx + dz * dz > VIEW_CHUNKS_SQUARED then
					continue
				end

				local nx = cx + dx
				local nz = cz + dz

				if nx < MIN_CHUNK or nx > MAX_CHUNK or nz < MIN_CHUNK or nz > MAX_CHUNK then
					continue
				end

				local key = ChunkKey(nx, nz)
				if chunks[key] then
					continue
				end

				chunks[key] = BuildChunk(nx, nz)
				loadedChunks += 1
				built += 1

				if built >= budget then
					return built
				end
			end
		end
	end

	return built
end

local function Evict(cx: number, cz: number)
	-- Collected before removing, because a table cannot be modified while it is
	-- being traversed with `pairs` and a streaming loop is exactly where that
	-- would go unnoticed until a rare rehash.
	local stale: { string } = {}

	for key, _ in chunks do
		local separator = string.find(key, ":", 1, true)
		if not separator then
			continue
		end

		local nx = tonumber(string.sub(key, 1, separator - 1))
		local nz = tonumber(string.sub(key, separator + 1))
		if not nx or not nz then
			continue
		end

		local dx = nx - cx
		local dz = nz - cz

		if dx * dx + dz * dz > RELEASE_CHUNKS_SQUARED then
			table.insert(stale, key)
		end
	end

	for _, key in stale do
		local chunk = chunks[key]
		if chunk then
			ReleaseChunk(chunk)
			chunks[key] = nil
			loadedChunks -= 1
		end
	end
end

-- --- the camera ------------------------------------------------------------

-- A slow circuit over the map, so the streaming is exercised rather than
-- described. The radius is large enough that the path leaves the region it
-- started in — a camera orbiting inside one loaded square proves nothing.
local PATH_RADIUS = 900
local PATH_PERIOD = 240
local EYE_CLEARANCE = 34

local function EyeAt(seconds: number): (Vector3, Vector3)
	local angle = seconds * (math.pi * 2 / PATH_PERIOD)

	local x = math.cos(angle) * PATH_RADIUS
	local z = math.sin(angle) * PATH_RADIUS

	-- Looking along the path rather than at its centre, so what is ahead is
	-- what is being streamed in and a late chunk is visible as one.
	local aheadX = math.cos(angle + 0.35) * PATH_RADIUS
	local aheadZ = math.sin(angle + 0.35) * PATH_RADIUS

	-- Above the ground *or* above the water, whichever is higher: over an ocean
	-- the terrain is far below and a camera that tracked it would fly at sea
	-- level through the waves.
	local ground = math.max(HeightAt(x, z), SEA_LEVEL)
	local aheadGround = math.max(HeightAt(aheadX, aheadZ), SEA_LEVEL)

	return Vector3.new(x, ground + EYE_CLEARANCE, z),
		Vector3.new(aheadX, aheadGround + EYE_CLEARANCE * 0.35, aheadZ)
end

local eye = Instance.new("Camera")
eye.Name = "Surveyor"
eye.FieldOfView = 75

-- The far plane has to clear the loaded region or the far chunks are built and
-- then clipped — which looks exactly like a streaming bug and is not one.
eye.FarPlaneZ = VIEW_CHUNKS * CHUNK * BLOCK * 2.2

local startPosition, startTarget = EyeAt(0)
eye.CFrame = CFrame.lookAt(startPosition, startTarget)
eye.Parent = workspace
workspace.CurrentCamera = eye

-- Fog ending just inside the loaded radius. **This is honesty rather than
-- decoration**: the built region has a square edge, and past it is nothing. Fog
-- puts the horizon where the data ends, so the edge is never on screen.
Lighting.FogColor = Color3.fromRGB(170, 196, 214)
Lighting.FogStart = VIEW_CHUNKS * CHUNK * BLOCK * 0.45
Lighting.FogEnd = VIEW_CHUNKS * CHUNK * BLOCK * 0.95
Lighting.OutdoorAmbient = Color3.fromRGB(112, 120, 132)

-- --- the music ------------------------------------------------------------
--
-- **Parented to `workspace`, which is what makes it music rather than a noise
-- something makes.** A `Sound` inside a part is heard from that part and falls
-- off with distance; one under a service is heard everywhere at one level. A
-- five-minute track emitting from a single point in a 16384-block world would
-- be inaudible for almost the whole flight, which is the wrong shape for a
-- score.
--
-- `SoundId` is the manifest's name, extension included, exactly as `MeshId`
-- names a mesh — so this plays when the client is pointed at a store that has
-- it published and is silent when it is not. The row keeps asking, so setting
-- `Playing` before the content has streamed is fine and it starts on the frame
-- the asset lands.
local music = Instance.new("Sound")
music.Name = "Score"
music.SoundId = "audio/lilium-lainu.mp3"
music.Looped = true
music.Volume = 0.6
music.Parent = workspace
music.Playing = true

-- --- the beat --------------------------------------------------------------

-- The ring around the camera, before the first frame. Everything else streams,
-- but a scene that presented a frame of empty sky before anything appeared
-- would also hand `LoadScene` a world one unit across — the bounds are measured
-- from what the script built, and at this point that is a camera.
Stream(ChunkOf(startPosition.X), ChunkOf(startPosition.Z), PREFILL_CHUNKS)

local reported = 0

RunService.Heartbeat:Connect(function(_deltaTime: number)
	-- `time()` rather than an accumulator over the delta. Both are simulated
	-- here, but the accumulator drifts by a float epsilon per beat and the
	-- clock does not — and a camera path that differs in the last bit is a
	-- recording that stops replaying.
	local seconds = time()

	local position, target = EyeAt(seconds)
	eye.CFrame = CFrame.lookAt(position, target)

	local cx = ChunkOf(position.X)
	local cz = ChunkOf(position.Z)

	Stream(cx, cz, CHUNKS_PER_BEAT)
	Evict(cx, cz)

	-- Once every ten seconds of simulated time, so the log says whether the
	-- working set is settling without a line per beat.
	if seconds - reported >= 10 then
		reported = seconds
		print(
			`terrain: chunk ({cx}, {cz}), {loadedChunks} loaded, `
				.. `{created - destroyed} parts alive, {poolCount} idle`
		)
	end
end)

print(
	`terrain: {MAP_BLOCKS}x{MAP_BLOCKS} blocks of {BLOCK}m `
		.. `({MAP_BLOCKS * MAP_BLOCKS} columns), `
		.. `{CHUNK}-block chunks, {VIEW_CHUNKS}-chunk view`
)
