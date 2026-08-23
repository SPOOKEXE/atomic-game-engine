--!strict
-- An endless landscape, one vertex a stud, built as `EditableMesh` geometry by
-- this file and nothing else, with somebody standing on it.
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
-- **And it is endless again, which the heightfield rewrite gave up.** That
-- version sampled a fixed 512 x 512 grid into one array and built all of it: a
-- landscape you look at, with an edge. The field is a *function* now -
-- `HeightAt` takes a world coordinate and has no table behind it - so chunks
-- are built ahead of whoever is walking and dropped behind them, and the only
-- bound left is `VIEW`, which is a rendering budget rather than a world.
--
-- ## What to look at
--
--   * **The bands.** Water, shore sand, grass, upland scrub, bare rock and snow
--     each come out as their own colour. `assets::MeshVertex` has no colour
--     field, so this works by `engine::render::BuildMeshData` giving every distinct
--     *triangle* colour its own submesh run - a scene of one flat brown means
--     that split has regressed to the average it used to take.
--   * **The slopes.** A cliff is shaded as a cliff because the normals are
--     computed from the field's own gradient rather than left at the default,
--     so lighting varies across a face rather than per box.
--   * **The seams.** Two chunks that meet compute the same height for the
--     column they share, because they call the same pure function with the same
--     coordinate. A visible crack means `HeightAt` stopped being pure.
--   * **The edge that is not there.** The field is a function of a world
--     coordinate with no table behind it and no map to fall off, so walking in
--     any direction builds ground ahead and drops it behind. What bounds the
--     scene is `VIEW`, which is a rendering budget rather than a world.
--
-- ## Run it
--
--     client --script Terrain.luau --stats
--
-- Walking it needs a host that solves - the studio's Play, or `just host
-- --game` - because a client registers the character systems and not the
-- physics pipeline, which is the note `StressPhysics.luau` gives at length.
-- Through the client the camera flies the same landscape instead, and the
-- streaming is the same streaming.

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

-- **The field is a function and nothing else, which is what makes it
-- endless.** `HeightAt` takes a world coordinate and returns a height, with no
-- table behind it and no edge to fall off. Everything below is about deciding
-- which parts of that function to turn into meshes right now.
--
-- The scene this replaced sampled a fixed 512 x 512 grid into one array and
-- built all of it. That is a landscape you look at; this is one you walk into.

-- How the field is cut into meshes. **A chunk is a rendering decision and not a
-- terrain one**: the field is continuous whatever this is set to, and two
-- chunks that meet compute the same height for the column they share because
-- they call the same function with the same coordinate.
local CHUNK = 64

-- How far the built world reaches, in chunks each way from the one the subject
-- stands in. Three gives a 7 x 7 block of chunks - 448 studs across - and a
-- chunk is dropped once it is further than `KEEP`.
--
-- **`KEEP` is one further out than `VIEW` on purpose.** Walking back and forth
-- across a chunk boundary with the two equal would build and destroy the same
-- mesh every few steps, which is a hitch a second for standing still.
local VIEW = 3
local KEEP = VIEW + 1

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

-- **The bands come from the field's own reach rather than from a survey of what
-- was generated**, which is the one thing an endless field cannot have. The old
-- scene sampled its whole map and took the highest point it found; there is no
-- highest point here, so the ceiling is the one the constants above allow -
-- half the base swing plus the whole ridge - and the bands are fractions of the
-- span from the waterline to it.
--
-- It is a ceiling rather than a maximum, so the snow line sits a little above
-- where a survey would have put it. That is the right way to be wrong: a summit
-- that is white is a summit, and a landscape that is white to the shore is a
-- broken colour pass.
local CEILING = RELIEF * 0.5 + RIDGE
local FLOOR = -(RELIEF * 0.5 + RIDGE)
local LAND_SPAN = math.max(CEILING - SEA_LEVEL, 1)

local function Band(fraction: number): number
	return SEA_LEVEL + LAND_SPAN * fraction
end

-- --- one chunk's samples ---------------------------------------------------
--
-- **A chunk samples its own block, with a one-stud skirt.** A normal is a
-- central difference and a slope is the same measurement, so the sample at a
-- chunk's edge needs the sample one stud outside it. Sampling `CHUNK + 3`
-- columns and offsetting by one gives every corner its neighbours without
-- reaching into another chunk's table - and because `HeightAt` is a pure
-- function, the skirt agrees exactly with the chunk next door.
--
-- The cost is the skirt: a 64-chunk samples 67 x 67 = 4,489 heights to build
-- 65 x 65 = 4,225 vertices, so six per cent of the noise is computed twice. The
-- alternative is a shared table across chunks, which is the thing that has to
-- go for the world to be endless.
local SKIRT = 1
local SPAN = CHUNK + 1 + SKIRT * 2

local function SampleBlock(baseX: number, baseZ: number): { number }
	local block = table.create(SPAN * SPAN, 0)
	for row = 0, SPAN - 1 do
		local z = baseZ + row - SKIRT
		local offset = row * SPAN
		for column = 0, SPAN - 1 do
			block[offset + column + 1] = HeightAt(baseX + column - SKIRT, z)
		end
	end
	return block
end

-- A sample from a block, in chunk-local coordinates where `(0, 0)` is the
-- chunk's own first corner and `-1` and `CHUNK + 1` are the skirt.
local function At(block: { number }, gx: number, gz: number): number
	return block[(gz + SKIRT) * SPAN + (gx + SKIRT) + 1]
end

-- The surface normal at a sample, from the field's own gradient.
--
-- Central differences over one stud each way. The cross product of the two
-- tangents `(2, dy/dx, 0)` and `(0, dy/dz, 2)` is `(-dy/dx, 2, -dy/dz)` once
-- the constant is divided out, which is what this returns - normalised, because
-- `EditableMesh` stores whatever it is given and the shader does not.
local function NormalAt(block: { number }, gx: number, gz: number): Vector3
	local dx = At(block, gx + 1, gz) - At(block, gx - 1, gz)
	local dz = At(block, gx, gz + 1) - At(block, gx, gz - 1)
	return Vector3.new(-dx, 2, -dz).Unit
end

-- How steep the surface is at a sample, as the drop across one stud. Used to
-- put rock on a cliff face whatever height the cliff is at, which is what stops
-- a mountainside from being grass up to the snow line.
local function SlopeAt(block: { number }, gx: number, gz: number): number
	local dx = At(block, gx + 1, gz) - At(block, gx - 1, gz)
	local dz = At(block, gx, gz + 1) - At(block, gx, gz - 1)
	return math.sqrt(dx * dx + dz * dz) * 0.5
end

-- The band a sample falls in.
--
-- **Height first, slope as an override.** Bands read as bands - a shoreline is
-- a line, a snow cap is a cap - and that is a function of altitude. Slope is
-- the one thing that cuts across them: anything steeper than about forty-five
-- degrees is bare because nothing grows on it and nothing settles on it, which
-- is why rock appears at every altitude and snow does not appear on a cliff.
local function ColourAt(block: { number }, gx: number, gz: number): Color3
	local height = At(block, gx, gz)
	local slope = SlopeAt(block, gx, gz)

	if height < SEA_LEVEL - (SEA_LEVEL - FLOOR) * 0.35 then
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
local dropped = 0
local triangles = 0

-- What is on screen, by chunk coordinate, so eviction is a lookup and a
-- `Destroy` rather than a walk of `Workspace`.
--
-- **The mesh is held beside the part and destroyed with it.** An
-- `EditableMesh` is an instance with no parent - `MeshPart.MeshId` names it by
-- content id rather than holding it - so destroying the part leaves the mesh
-- behind, and an endless world would leave one behind per chunk for as long as
-- somebody kept walking. That is a hundred and fifty kilobytes a chunk of
-- positions, normals and colours that nothing will ever draw again.
type Chunk = { Part: Instance, Mesh: Instance }

local chunks: { [string]: Chunk } = {}

-- Chunks whose mesh was built this frame and whose collision shape is baked at
-- the start of the next tick. See `BuildChunk`.
type Settling = { Part: Instance, Geometry: string }

local settling: { Settling } = {}

-- Turns on the collision of every chunk built before this frame.
--
-- One frame of lag and no more: the bake runs in `PreSimulation` and a chunk is
-- built from `Heartbeat`, so by the time this is reached again the shape is
-- there. A chunk that is destroyed in between is simply not in the list.
local function Settle()
	if #settling == 0 then
		return
	end

	for _, waiting in settling do
		local part = waiting.Part :: any
		if part.Parent ~= nil then
			part.CollisionShape = Enum.ShapeKind.Mesh
			part.CollisionGeometry = waiting.Geometry
			part.CanCollide = true
		end
	end
	table.clear(settling)
end

local function KeyOf(cx: number, cz: number): string
	return `{cx},{cz}`
end

-- One chunk: `CHUNK + 1` samples a side, so it shares its first and last
-- columns with the chunks either side of it and the surface is continuous.
--
-- The vertices are written in **world** coordinates and the part is then given
-- the mesh's own bounding box as its `Size` and the box's centre as its
-- `Position`. `render::Renderer`'s `ToGpu` scales a mesh so its box fills the
-- part's, so a part sized to the box it holds is a scale of exactly one - which
-- is the only arrangement where a heightfield is not quietly stretched.
local function BuildChunk(cx: number, cz: number)
	local baseX = cx * CHUNK
	local baseZ = cz * CHUNK
	local block = SampleBlock(baseX, baseZ)
	local span = CHUNK + 1

	-- **The range first, because the vertices are written around the centre.**
	-- A mesh's own coordinates are what a collider is placed by: a baked shape
	-- is used in the part's object space and is not scaled to it, which
	-- `physics::ShapeInstance` states. So the mesh is built centred on the box
	-- the part will be given, and the two coincide exactly.
	--
	-- The first version wrote world coordinates and relied on `ToGpu` scaling
	-- the mesh to fill the part - which draws correctly and puts the collider a
	-- chunk's position away from the geometry it is supposed to be.
	local lowest = math.huge
	local highest = -math.huge
	for gz = 0, CHUNK do
		for gx = 0, CHUNK do
			local height = At(block, gx, gz)
			lowest = math.min(lowest, height)
			highest = math.max(highest, height)
		end
	end

	local centreX = baseX + CHUNK / 2
	local centreY = (lowest + highest) / 2
	local centreZ = baseZ + CHUNK / 2

	local mesh = Instance.new("EditableMesh")

	-- One transaction rather than three method calls per vertex and two more per
	-- cell. `SetGeometry` yields until the next deterministic script barrier,
	-- where this scene's pending chunks are prepared as one batch and committed
	-- on its world thread. Universe runs the same batch for other scenes in its
	-- outer world dispatch. The id of `(gx, gz)` remains `gz * span + gx`.
	local vertices = {}
	for gz = 0, CHUNK do
		for gx = 0, CHUNK do
			table.insert(vertices, {
				Position = Vector3.new(
					baseX + gx - centreX,
					At(block, gx, gz) - centreY,
					baseZ + gz - centreZ
				),
				Normal = NormalAt(block, gx, gz),
				Color = ColourAt(block, gx, gz),
			})
		end
	end

	-- Two triangles a cell, wound counter-clockwise seen from above so the
	-- surface faces the sky. `render/AGENTS.md`'s winding rule shows up as an
	-- invisible landscape when it is got wrong, which is a hard thing to debug
	-- backwards from.
	local indices = {}
	for gz = 0, CHUNK - 1 do
		for gx = 0, CHUNK - 1 do
			local a = gz * span + gx
			local b = a + 1
			local c = a + span
			local d = c + 1

			table.insert(indices, a)
			table.insert(indices, c)
			table.insert(indices, b)
			table.insert(indices, b)
			table.insert(indices, c)
			table.insert(indices, d)
			triangles += 2
		end
	end

	assert(mesh:SetGeometry(vertices, indices), "terrain mesh transaction was cancelled")

	-- **A floor under the vertical size.** A chunk that happens to be flat has
	-- a box with no height at all, and a `Size` of zero on an axis is the one
	-- case `ToGpu` cannot scale - it falls back to doubling, which would make a
	-- flat plain the only part of the map drawn at the wrong scale.
	local height = math.max(highest - lowest, 0.01)

	local part = Instance.new("MeshPart")
	part.Name = `Terrain_{cx}_{cz}`
	part.Anchored = true
	part.Size = Vector3.new(CHUNK, height, CHUNK)
	part.Position = Vector3.new(centreX, centreY, centreZ)
	part.MeshId = mesh.ContentId
	part.Parent = workspace

	-- **And it collides as the mesh, which is the whole of walking on it.** A
	-- collider names its geometry and picks its kind separately, and the
	-- default kind is a box - so a chunk left alone collides as its own bound,
	-- a 64-stud box the full height of the chunk, and a character standing on
	-- the surface is *inside* it and cannot move in any direction. That is what
	-- happened the first time this scene had somebody in it.
	--
	-- `Mesh` and not `Hull`: a hill's convex hull is a dome over its summit, so
	-- every valley in the chunk would be solid ground well above the drawn one.
	-- `physics::RegisterPhysicsSystems` bakes both out of the `EditableMesh` on
	-- the tick it changes - `scene::RefreshEditableMeshCollision` - so naming
	-- either costs nothing here.
	--
	-- **Not this frame, and that is the one subtlety.** The shape is baked by
	-- `scene::RefreshEditableMeshCollision` on the tick after the mesh is
	-- built, and a collider naming a shape that has not resolved yet collides
	-- as the part's own bound - which for a chunk is a solid 64-stud box the
	-- full height of it. A character standing where one appears is thrown onto
	-- the roof of a box that vanishes a tick later. So the chunk does not
	-- collide until its shape exists, and `Settle` below is where it starts.
	--
	-- After the parent, because a collider is what these properties write and
	-- there is none until the part is in the world.
	part.CanCollide = false
	table.insert(settling, { Part = part, Geometry = mesh.ContentId })

	chunks[KeyOf(cx, cz)] = { Part = part, Mesh = mesh }
	built += 1
end

-- --- the water -------------------------------------------------------------
--
-- One slab rather than a second mesh. The sea has no relief to carry, and a
-- flat translucent surface is what separates "the low ground is blue" from
-- "there is water in the low ground" - which is the whole reason the two
-- submerged bands are coloured at all.
--
-- **It follows the subject**, because an endless landscape cannot have a slab
-- the size of itself. Wide enough to reach past the built chunks in every
-- direction, and moved in whole chunks so it never appears to slide.
local WATER_REACH = (KEEP * 2 + 3) * CHUNK

local water = Instance.new("Part")
water.Name = "Sea"
water.Anchored = true
water.CanCollide = false
water.Size = Vector3.new(WATER_REACH, 0.4, WATER_REACH)
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

-- **Fog where the chunks stop**, which is the one job it has here. The built
-- world reaches `VIEW` chunks each way, so a fog that closes just inside that
-- hides the edge of the data instead of bleaching the bands.
Lighting.FogColor = Color3.fromRGB(178, 200, 218)
Lighting.FogStart = VIEW * CHUNK * 0.8
Lighting.FogEnd = VIEW * CHUNK * 1.35

-- --- somebody to walk it ---------------------------------------------------
--
-- **A spawn on the surface, not at the origin.** `LoadCharacter` puts a
-- character at whatever `scene::FindSpawn` finds, and a spawn left at `y = 0`
-- in a field that swings sixty studs either way is a character either buried in
-- a hill or dropped off one. The pad sits on the field's own height at the
-- origin, and the character stands on the pad.
-- **Somewhere flat, found rather than assumed.** The field is 91% walkable and
-- the origin is not guaranteed to be part of it: a character dropped onto a
-- 58-degree face slides off it before anybody has touched a key. The search is
-- a coarse spiral outward from the origin, taking the first place that is above
-- the water and level enough to stand on - `MINIMUM_WALKABLE_NORMAL` in
-- `physics::Characters` is the same 0.6 the engine uses to decide what a
-- character may climb.
local function LevelAt(x: number, z: number): number
	local dx = HeightAt(x + 1, z) - HeightAt(x - 1, z)
	local dz = HeightAt(x, z + 1) - HeightAt(x, z - 1)
	return (Vector3.new(-dx, 2, -dz).Unit).Y
end

local spawnX, spawnZ = 0, 0
for radius = 0, 30 do
	local found = false
	for step = 0, math.max(radius * 8, 1) - 1 do
		local angle = step * math.pi * 2 / math.max(radius * 8, 1)
		local x = math.cos(angle) * radius * 6
		local z = math.sin(angle) * radius * 6
		if HeightAt(x, z) > SEA_LEVEL + 3 and LevelAt(x, z) > 0.9 then
			spawnX, spawnZ = x, z
			found = true
			break
		end
	end
	if found then
		break
	end
end

local SPAWN_HEIGHT = math.max(HeightAt(spawnX, spawnZ), SEA_LEVEL + 2)

-- **Sitting on the surface rather than sunk into it.** `FindSpawn` puts a
-- character's feet on the pad, so the pad's own top is where the character
-- starts - and a pad buried in the ground starts a character inside the hill,
-- which the solver answers by throwing it into the air.
local pad = Instance.new("SpawnLocation")
pad.Name = "SpawnLocation"
pad.Anchored = true
pad.Size = Vector3.new(8, 1, 8)
pad.Position = Vector3.new(spawnX, SPAWN_HEIGHT + 0.5, spawnZ)
pad.Color = Color3.fromRGB(120, 120, 130)
pad.Parent = workspace

local Players = game:GetService("Players")

local walker = Instance.new("Player")
walker.Name = "Surveyor"
walker.Parent = Players
walker:LoadCharacter()

-- **Walking it needs a host that installs the physics pipeline**, which is the
-- note `StressPhysics.luau` gives at more length and for the same reason: a
-- client registers the *character* systems only, so a scripted client world
-- integrates nothing and the body stands wherever it was put. The studio's Play
-- and `just host --game` both solve, and there the character walks and the
-- world streams around it.
local function CharacterRoot(): Instance?
	local model = workspace:FindFirstChild("Surveyor")
	return if model ~= nil then model:FindFirstChild("HumanoidRootPart") else nil
end

-- --- the camera ------------------------------------------------------------
--
-- **A flight rather than an orbit, because there is nothing to orbit.** The
-- scene this replaced circled a 512-stud island and looked in at it. An endless
-- field has no middle, so the camera goes somewhere instead: forward at a
-- walking pace, holding its height above whatever ground is under it, which is
-- the view that shows chunks arriving ahead and falling away behind.
--
-- **It flies until the character moves, and then it is the character's.** That
-- is one test for the question this scene cannot ask directly - `IsServer` and
-- `IsClient` report the same thing in a standalone client as in a dedicated
-- server, so a script cannot ask whether a host solves. A body that has moved
-- has been solved by something; a body still on its pad has not, and a scene
-- that stood still there would show nothing of what it is about.
local FLIGHT_SPEED = 26
local FLIGHT_HEIGHT = 46
local FLIGHT_LOOK = 90

local eye = Instance.new("Camera")
eye.Name = "Flyer"
eye.FieldOfView = 62
eye.FarPlaneZ = VIEW * CHUNK * 4
eye.CFrame = CFrame.lookAt(
	Vector3.new(0, SPAWN_HEIGHT + FLIGHT_HEIGHT, 0),
	Vector3.new(FLIGHT_LOOK, SPAWN_HEIGHT, 0)
)
eye.Parent = workspace
workspace.CurrentCamera = eye

local flying = true
local spawned: Vector3? = nil

-- **Ticks rather than `time()`, because two runs of this scene have to build
-- the same world.** The flight decides which chunks exist, so a camera placed
-- from the wall clock would put a different set of meshes in the world on every
-- run - and `HeightAt` being a pure function of its seed would stop being
-- something anybody could check. `examples/tests/Scene.cpp` checks it.
local flown = 0

local function FlyCamera()
	local root = CharacterRoot()
	if root ~= nil then
		local at = (root :: any).Position
		if spawned == nil then
			spawned = at
		elseif ((at - (spawned :: Vector3)).Magnitude) > 0.25 then
			-- Something is solving. Stop writing the camera and let whatever
			-- follows the character have it - two writers and the last one
			-- wins, which reads as a camera fighting itself.
			flying = false
			return
		end
	end

	flown += 1
	local travelled = flown * FLIGHT_SPEED / 60
	local ahead = travelled + FLIGHT_LOOK

	-- Above the ground rather than at a fixed altitude, so a mountain does not
	-- swallow the camera and a valley does not leave it in the sky.
	local ground = math.max(HeightAt(travelled, 0), SEA_LEVEL)
	local aim = math.max(HeightAt(ahead, 0), SEA_LEVEL)

	eye.CFrame = CFrame.lookAt(
		Vector3.new(travelled, ground + FLIGHT_HEIGHT, 0),
		Vector3.new(ahead, aim + FLIGHT_HEIGHT * 0.25, 0)
	)
end

-- What the streaming follows: the character once something is moving it, and
-- the camera until then.
local function SubjectPosition(): Vector3
	if not flying then
		local root = CharacterRoot()
		if root ~= nil then
			return (root :: any).Position
		end
	end

	return eye.CFrame.Position
end

-- --- streaming --------------------------------------------------------------
--
-- **One chunk a frame, nearest first.** The mesh calls for a 64-chunk are about
-- twenty thousand, so building a ring of them in one frame is a stall you can
-- see; building the nearest missing one each frame fills the world in front of
-- whoever is walking into it and leaves the far corners for later.
--
-- The whole of "infinite" is here: `HeightAt` has no edge, so the only question
-- each frame is which chunks are worth holding.
local function ChunkOf(value: number): number
	return math.floor(value / CHUNK)
end

local function Stream()
	local at = SubjectPosition()
	local cx = ChunkOf(at.X)
	local cz = ChunkOf(at.Z)

	-- The nearest chunk that is wanted and not built. Squared distance in chunk
	-- units, so the ring fills roughly outward rather than row by row.
	local wantX: number? = nil
	local wantZ: number? = nil
	local nearest = math.huge

	for dz = -VIEW, VIEW do
		for dx = -VIEW, VIEW do
			local key = KeyOf(cx + dx, cz + dz)
			if chunks[key] == nil then
				local distance = dx * dx + dz * dz
				if distance < nearest then
					nearest = distance
					wantX = cx + dx
					wantZ = cz + dz
				end
			end
		end
	end

	if wantX ~= nil and wantZ ~= nil then
		BuildChunk(wantX, wantZ)
		-- One a frame, so the eviction below waits for the next one. A frame
		-- that both builds and destroys is the worst of both.
		return
	end

	-- Nothing left to build, so drop whatever has fallen out of `KEEP`.
	for key, chunk in chunks do
		local comma = string.find(key, ",", 1, true)
		local kx = tonumber(string.sub(key, 1, (comma :: number) - 1)) :: number
		local kz = tonumber(string.sub(key, (comma :: number) + 1)) :: number
		if math.abs(kx - cx) > KEEP or math.abs(kz - cz) > KEEP then
			chunk.Part:Destroy()
			chunk.Mesh:Destroy()
			chunks[key] = nil
			dropped += 1
		end
	end

	water.Position = Vector3.new(cx * CHUNK + CHUNK / 2, SEA_LEVEL, cz * CHUNK + CHUNK / 2)
end

-- The first ring is built before the first frame is drawn, so a character does
-- not spawn into an empty world and fall through where the ground has not been
-- made yet. Only the chunk under the spawn and its immediate neighbours - the
-- rest arrives at one a frame like everything else.
for dz = -1, 1 do
	for dx = -1, 1 do
		BuildChunk(dx, dz)
	end
end

local announced = false
local beat = 0
local streaming = false

local function CountChunks(): number
	local resident = 0
	for _ in chunks do
		resident += 1
	end
	return resident
end

RunService.Heartbeat:Connect(function()
	if flying then
		FlyCamera()
	end

	Settle()
	if not streaming then
		-- Signal callbacks are independent coroutines. `BuildChunk` waits for its
		-- mesh commit, so another Heartbeat may arrive before this one returns.
		-- One producer keeps two callbacks from generating the same missing tile.
		streaming = true
		Stream()
		streaming = false
	end

	if not announced and built >= (VIEW * 2 + 1) * (VIEW * 2 + 1) then
		announced = true
		print(`terrain: {built} chunk mesh(es), {triangles} triangle(s), first ring complete`)
	end

	-- **Every sixty ticks while it is moving, because "endless" is a claim
	-- about what happens after the first ring.** A scene that built its ring
	-- and then did nothing would look identical to the fixed map this replaced,
	-- and this line is the difference: chunks keep being built and dropped for
	-- as long as the subject travels.
	beat += 1
	if beat % 60 == 0 then
		local at = SubjectPosition()
		print(
			`terrain: at {string.format("%.0f", at.X)},{string.format("%.0f", at.Z)} · `
				.. `{built} built, {dropped} dropped, {CountChunks()} resident`
		)
	end
end)

print(
	`terrain: endless field, chunks of {CHUNK} studs, {VIEW} chunks each way `
		.. `({(VIEW * 2 + 1) * CHUNK} studs across), `
		.. `character spawned at {string.format("%.0f", spawnX)},{string.format("%.0f", spawnZ)} `
		.. `y={string.format("%.1f", SPAWN_HEIGHT)}`
)
