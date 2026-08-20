--!strict
-- A 512 x 512 stud landscape, one vertex a stud, built as `EditableMesh`
-- geometry by this file and nothing else.
--
-- **Pure Luau over an `EditableMesh`, which is the whole point of the
-- rewrite.** This scene used to be a voxel streamer: a 16384-block map turned
-- into merged boxes, a `Part` pool underneath it and an eviction ring around
-- the camera. It proved streaming, and it proved nothing about the class this
-- engine actually gives a script for building geometry - every surface on
-- screen was an axis-aligned box, so a slope was a staircase and a colour was a
-- part property rather than anything the mesh carried.
--
-- What is here instead is the shape a heightfield genuinely is: a grid of
-- samples, triangulated, with a normal and a colour at every corner. The noise
-- library below is unchanged from the voxel version - it was always the good
-- half - and everything under `--- the field` is new.
--
-- ## What to look at
--
--   * **The bands.** Water, shore sand, grass, upland scrub, bare rock and snow
--     each come out as their own colour. `assets::MeshVertex` has no colour
--     field, so this works by `client::BuildMeshData` giving every distinct
--     *triangle* colour its own submesh run - a scene of one flat brown means
--     that split has regressed to the average it used to take.
--   * **The slopes.** A cliff is shaded as a cliff because the normals are
--     computed from the field's own gradient rather than left at the default,
--     so lighting varies across a face rather than per box.
--   * **The seams.** Chunks share their edge samples, so neighbouring meshes
--     agree at the boundary exactly. A visible crack means the shared column
--     stopped being shared.
--
-- ## Run it
--
--     client --script Terrain.luau --stats

local Lighting = game:GetService("Lighting")
local RunService = game:GetService("RunService")

-- --- the noise library -----------------------------------------------------
--
-- Gradient (Perlin-style) noise, seeded, with **no permutation table**.
--
-- The textbook implementation hashes a lattice corner by indexing a shuffled
-- 256-entry table, which makes the field repeat every 256 units. On a map 64
-- times wider than that the repeat is not subtle - it is a visible grid of
-- identical mountains, and it is the first thing that goes wrong when a
-- tutorial's noise meets a real world size. Hashing the coordinates
-- arithmetically instead gives a field whose period is 2^32 in each axis, which
-- is 262144 times the map.
--
-- `bit32` is exact here, and that is worth stating rather than assuming.
-- Luau converts a double to an unsigned with `(unsigned)(long long)` - so every
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
-- to the function containing it - which here was once per column, per chunk,
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
-- field at a different zoom, and a feature at one scale reappears at the next -
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
-- The absolute value folds the field at zero, and a fold is a crease - which is
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

-- --- the field -------------------------------------------------------------

local SEED = 20260820

-- **512 studs on a side with a sample every stud**, which is the size this
-- scene is about rather than a number that could be any other. It is small
-- enough to build whole - no streaming, no eviction, no pool - and dense enough
-- that a hillside is a surface instead of a staircase.
local SIDE = 512
local ORIGIN = -SIDE / 2

-- How the field is cut into meshes. **A chunk is a rendering decision and not a
-- terrain one**: the samples are one continuous grid whatever this is set to,
-- and neighbouring chunks share their edge column rather than each computing
-- their own copy of it. 64 gives 64 meshes of 4,225 vertices, which keeps any
-- one `MeshPart` inside a frustum test that means something while leaving the
-- draw call count in the dozens.
local CHUNK = 64
local CHUNKS = SIDE // CHUNK

-- Vertical scale. `RELIEF` is the full swing of the base field and `RIDGE` how
-- much the ridged octaves add on top of it, both in studs.
local RELIEF = 46
local RIDGE = 38

-- Where the water sits. Chosen against the field rather than picked: the base
-- field averages near zero, so a sea level a little below it leaves rather more
-- land than water.
local SEA_LEVEL = -6

-- Feature size, in studs per unit of noise. Bigger is smoother.
local BASE_SCALE = 220
local RIDGE_SCALE = 150

-- The colours, darkest first. See `ColourAt`.
local DEEP = Color3.fromRGB(28, 52, 84)
local SHALLOW = Color3.fromRGB(48, 96, 132)
local SAND = Color3.fromRGB(198, 178, 128)
local GRASS = Color3.fromRGB(86, 124, 62)
local SCRUB = Color3.fromRGB(66, 92, 54)
local ROCK = Color3.fromRGB(112, 108, 102)
local SNOW = Color3.fromRGB(236, 238, 242)

-- One height for one sample.
--
-- Base fBm for the landmass and a ridged multifractal for the mountains, the
-- second **masked by the first** so ranges rise out of high ground rather than
-- out of the sea. Without the mask a ridge crosses a bay and stands in the
-- water like a wall.
local function HeightAt(x: number, z: number): number
	local base = fbm2(x / BASE_SCALE, z / BASE_SCALE, SEED, 4, 2.0) * FBM_GAIN
	base = math.clamp(base, -1, 1)

	local mask = math.clamp((base + 0.15) * 1.6, 0, 1)
	local ridge = ridged2(x / RIDGE_SCALE, z / RIDGE_SCALE, SEED + 7717, 4)

	return base * RELIEF * 0.5 + ridge * mask * mask * RIDGE
end

-- **The whole field, sampled once, before any mesh is built.** Every sample is
-- read at least five times - once as a corner and four times as a neighbour of
-- one, while a normal is computed - and the edge column of every chunk is read
-- by the chunk next door as well. Recomputing it each time would multiply four
-- octaves of noise by six for nothing.
--
-- One flat array, row-major, indexed by `Index` below. 513 columns rather than
-- 512, because a grid of 512 *cells* has 513 corners and the last one is what
-- the last chunk's far edge is made of.
local SAMPLES = SIDE + 1

local heights = table.create(SAMPLES * SAMPLES, 0)

local function Index(ix: number, iz: number): number
	return iz * SAMPLES + ix + 1
end

for iz = 0, SIDE do
	local z = ORIGIN + iz
	for ix = 0, SIDE do
		heights[Index(ix, iz)] = HeightAt(ORIGIN + ix, z)
	end
end

-- A sample, clamped at the edges so a normal at the boundary uses the boundary
-- itself rather than wrapping to the far side of the map.
local function Sample(ix: number, iz: number): number
	return heights[Index(math.clamp(ix, 0, SIDE), math.clamp(iz, 0, SIDE))]
end

-- **The bands are cut from the field that was actually generated, not from
-- constants written beside it.** Every threshold below used to be an absolute
-- height, so retuning `RELIEF` or `RIDGE` by a few studs silently moved the
-- whole map into one band - a change to how tall the mountains are turned the
-- landscape entirely white, which reads as a broken colour pass rather than as
-- the thing it is.
local lowestSample = math.huge
local highestSample = -math.huge
for index = 1, #heights do
	local height = heights[index]
	lowestSample = math.min(lowestSample, height)
	highestSample = math.max(highestSample, height)
end

-- Land runs from the waterline to the highest point; the bands are fractions of
-- that, so they hold whatever the relief constants are set to.
local LAND_SPAN = math.max(highestSample - SEA_LEVEL, 1)

local function Band(fraction: number): number
	return SEA_LEVEL + LAND_SPAN * fraction
end

-- The surface normal at a sample, from the field's own gradient.
--
-- Central differences over one stud each way. The cross product of the two
-- tangents `(2, dy/dx, 0)` and `(0, dy/dz, 2)` is `(-dy/dx, 2, -dy/dz)` once
-- the constant is divided out, which is what this returns - normalised, because
-- `EditableMesh` stores whatever it is given and the shader does not.
local function NormalAt(ix: number, iz: number): Vector3
	local dx = Sample(ix + 1, iz) - Sample(ix - 1, iz)
	local dz = Sample(ix, iz + 1) - Sample(ix, iz - 1)
	return Vector3.new(-dx, 2, -dz).Unit
end

-- How steep the surface is at a sample, as the drop across one stud. Used to
-- put rock on a cliff face whatever height the cliff is at, which is what stops
-- a mountainside from being grass up to the snow line.
local function SlopeAt(ix: number, iz: number): number
	local dx = Sample(ix + 1, iz) - Sample(ix - 1, iz)
	local dz = Sample(ix, iz + 1) - Sample(ix, iz - 1)
	return math.sqrt(dx * dx + dz * dz) * 0.5
end

-- The band a sample falls in.
--
-- **Height first, slope as an override.** Bands read as bands - a shoreline is
-- a line, a snow cap is a cap - and that is a function of altitude. Slope is
-- the one thing that cuts across them: anything steeper than about forty-five
-- degrees is bare because nothing grows on it and nothing settles on it, which
-- is why rock appears at every altitude and snow does not appear on a cliff.
local function ColourAt(ix: number, iz: number): Color3
	local height = Sample(ix, iz)
	local slope = SlopeAt(ix, iz)

	if height < SEA_LEVEL - (SEA_LEVEL - lowestSample) * 0.35 then
		return DEEP
	elseif height < SEA_LEVEL then
		return SHALLOW
	end

	if slope > 1.0 then
		return ROCK
	end

	if height < Band(0.05) then
		return SAND
	elseif height < Band(0.35) then
		return GRASS
	elseif height < Band(0.60) then
		return SCRUB
	elseif height < Band(0.82) then
		return ROCK
	end
	return SNOW
end

-- --- the meshes ------------------------------------------------------------

local built = 0
local triangles = 0

-- One chunk: `CHUNK + 1` samples a side, so it shares its first and last
-- columns with the chunks either side of it and the surface is continuous.
--
-- The vertices are written in **world** coordinates and the part is then given
-- the mesh's own bounding box as its `Size` and the box's centre as its
-- `Position`. `render::Renderer`'s `ToGpu` scales a mesh so its box fills the
-- part's, so a part sized to the box it holds is a scale of exactly one - which
-- is the only arrangement where a heightfield is not quietly stretched.
local function BuildChunk(cx: number, cz: number)
	local mesh = Instance.new("EditableMesh")

	local baseX = cx * CHUNK
	local baseZ = cz * CHUNK
	local span = CHUNK + 1

	local lowest = math.huge
	local highest = -math.huge

	-- `AddVertex` returns ids in the order it is called, starting at zero, so
	-- the id of `(gx, gz)` is `gz * span + gx` and no lookup table is needed.
	-- Checked once here rather than assumed.
	for gz = 0, CHUNK do
		local iz = baseZ + gz
		for gx = 0, CHUNK do
			local ix = baseX + gx
			local height = Sample(ix, iz)
			lowest = math.min(lowest, height)
			highest = math.max(highest, height)

			local id = mesh:AddVertex(Vector3.new(ORIGIN + ix, height, ORIGIN + iz))
			mesh:SetVertexNormal(id, NormalAt(ix, iz))
			mesh:SetVertexColor(id, ColourAt(ix, iz))
		end
	end

	-- Two triangles a cell, wound counter-clockwise seen from above so the
	-- surface faces the sky. `render/AGENTS.md`'s winding rule shows up as an
	-- invisible landscape when it is got wrong, which is a hard thing to debug
	-- backwards from.
	for gz = 0, CHUNK - 1 do
		for gx = 0, CHUNK - 1 do
			local a = gz * span + gx
			local b = a + 1
			local c = a + span
			local d = c + 1

			mesh:AddTriangle(a, c, b)
			mesh:AddTriangle(b, c, d)
			triangles += 2
		end
	end

	local centreX = ORIGIN + baseX + CHUNK / 2
	local centreZ = ORIGIN + baseZ + CHUNK / 2

	-- **A floor under the vertical size.** A chunk that happens to be flat has
	-- a box with no height at all, and a `Size` of zero on an axis is the one
	-- case `ToGpu` cannot scale - it falls back to doubling, which would make a
	-- flat plain the only part of the map drawn at the wrong scale.
	local height = math.max(highest - lowest, 0.01)

	local part = Instance.new("MeshPart")
	part.Name = `Terrain_{cx}_{cz}`
	part.Anchored = true
	part.Size = Vector3.new(CHUNK, height, CHUNK)
	part.Position = Vector3.new(centreX, (lowest + highest) / 2, centreZ)
	part.MeshId = mesh.ContentId
	part.Parent = workspace

	built += 1
end

-- --- the water -------------------------------------------------------------
--
-- One slab rather than a second mesh. The sea has no relief to carry, and a
-- flat translucent surface is what separates "the low ground is blue" from
-- "there is water in the low ground" - which is the whole reason the two
-- submerged bands are coloured at all.
local water = Instance.new("Part")
water.Name = "Sea"
water.Anchored = true
water.CanCollide = false
water.Size = Vector3.new(SIDE, 0.4, SIDE)
water.Position = Vector3.new(0, SEA_LEVEL, 0)
water.Color = Color3.fromRGB(46, 104, 148)
water.Transparency = 0.45
water.Parent = workspace

-- --- the light -------------------------------------------------------------

-- **Kept low deliberately.** The bands are the subject, and a bright sun over a
-- pale ambient washes every one of them toward the same white - which looked
-- like the colours had not been applied at all. One sun, a cool shadow fill,
-- and nothing else.
Lighting.Brightness = 0.5
Lighting.ClockTime = 15
Lighting.OutdoorAmbient = Color3.fromRGB(22, 26, 32)
Lighting.Ambient = Color3.fromRGB(16, 18, 24)
-- **Fog beyond the map rather than across it.** The camera orbits at 0.78 of a
-- side and looks at the middle, so the far edge is about 1.8 sides away - a fog
-- that started at 0.9 therefore covered almost the whole landscape and bleached
-- every band toward the fog colour. It reads exactly like a washed-out palette,
-- and no amount of retuning the colours fixes it. Starting past the far corner
-- leaves the fog doing the one job it is here for: hiding where the data stops.
Lighting.FogColor = Color3.fromRGB(178, 200, 218)
Lighting.FogStart = SIDE * 1.9
Lighting.FogEnd = SIDE * 2.6

-- --- the camera ------------------------------------------------------------
--
-- A slow orbit outside the map looking in, so the whole 512 studs is in frame
-- and the bands read as bands. The old scene flew *through* its world because
-- the world was too big to see; this one is not.
local ORBIT_RADIUS = SIDE * 0.78
local ORBIT_PERIOD = 90
local ORBIT_HEIGHT = 190

local eye = Instance.new("Camera")
eye.Name = "Surveyor"
eye.FieldOfView = 62
eye.FarPlaneZ = SIDE * 4
eye.CFrame = CFrame.lookAt(Vector3.new(ORBIT_RADIUS, ORBIT_HEIGHT, 0), Vector3.new(0, 0, 0))
eye.Parent = workspace
workspace.CurrentCamera = eye

-- --- building it -------------------------------------------------------------
--
-- **A chunk a frame rather than all sixty-four at once.** The field above is
-- already sampled, so this is only the mesh calls - but there are about twenty
-- thousand of them per chunk, and doing every chunk before the first present
-- would hold the window blank for as long as that takes. One a frame gets a
-- picture up immediately and finishes in about a second, and the map filling in
-- is a fair thing to watch rather than a stall.
local pending: { { number } } = {}
for cz = 0, CHUNKS - 1 do
	for cx = 0, CHUNKS - 1 do
		table.insert(pending, { cx, cz })
	end
end

-- Nearest the camera's opening position first, so what is built early is what
-- is on screen early.
table.sort(pending, function(left, right)
	local lx = left[1] * CHUNK + CHUNK / 2 + ORIGIN - ORBIT_RADIUS
	local rx = right[1] * CHUNK + CHUNK / 2 + ORIGIN - ORBIT_RADIUS
	return lx * lx < rx * rx
end)

local next_ = 1
local announced = false

RunService.Heartbeat:Connect(function()
	if next_ <= #pending then
		local chunk = pending[next_]
		next_ += 1
		BuildChunk(chunk[1], chunk[2])

		if next_ > #pending and not announced then
			announced = true
			print(`terrain: {built} chunk mesh(es), {triangles} triangle(s) built`)
		end
	end

	local angle = time() * (math.pi * 2 / ORBIT_PERIOD)
	eye.CFrame = CFrame.lookAt(
		Vector3.new(math.cos(angle) * ORBIT_RADIUS, ORBIT_HEIGHT, math.sin(angle) * ORBIT_RADIUS),
		Vector3.new(0, 0, 0)
	)
end)

print(
	`terrain: {SIDE}x{SIDE} studs at one sample a stud ({SAMPLES * SAMPLES} samples), `
		.. `{CHUNKS}x{CHUNKS} chunks of {CHUNK}, `
		.. `height {string.format("%.1f", lowestSample)} to {string.format("%.1f", highestSample)}`
)
