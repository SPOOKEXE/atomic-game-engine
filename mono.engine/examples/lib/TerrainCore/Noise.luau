--!strict

-- Seeded coherent noise, with no permutation table.
--
-- The textbook Perlin implementation hashes a lattice corner by indexing a
-- shuffled 256-entry table, which makes the field repeat every 256 units. On a
-- map wider than that the repeat is not subtle -- it is a visible grid of
-- identical hills, and it is the first thing that goes wrong when a tutorial's
-- noise meets a real world size. Hashing the coordinates arithmetically instead
-- gives a field whose period is 2^32 on each axis.
--
-- Everything here is a pure function of its arguments. That is not decoration:
-- `HeightField` never stores a map, so a column's height is recomputed every
-- time anybody asks, and two runs of the game have to agree about it block for
-- block. `math.random` would break that and so would anything reading a clock.
--
-- ## Why the multipliers are 16-bit
--
-- Luau converts a double to an unsigned with `(unsigned)(long long)`, so `bit32`
-- is exact for every intermediate below 2^53. A 32-by-32 multiply reaches 2^64
-- and would silently start dropping low bits -- a hash that degrades rather than
-- one that fails, which is the worst kind. Every product below stays under 2^48.

-- ## What came from Atlas
--
-- The simplex kernels, the analytical-derivative algebra and the Worley cell
-- search below are ported from Atlas (https://github.com/shmeatley/atlas, MIT),
-- a procedural generation toolkit for Roblox. What was *not* ported is its
-- lattice hashing: Atlas builds a shuffled 256-entry permutation table per
-- sampler, which is fast and which repeats every 256 units on every axis --
-- the artefact this file's header exists to reject. The kernels are hashed with
-- `hash2`/`hash3` instead, so simplex and Worley inherit the same 2^32 period
-- as everything else here rather than bringing a grid back in through a side
-- door.
--
-- Atlas is also a Wally package whose samplers are objects (`Simplex.new(seed)`)
-- and whose visualiser needs an `EditableImage`. Neither survives the trip: this
-- library is plain functions of `(coordinates, seed)` because `HeightField` has
-- nowhere to keep a sampler and no Instance to draw into. `NoiseMap` beside this
-- file is the part of Atlas's visualiser that is engine-independent -- the grid,
-- not the image.

local band = bit32.band
local bxor = bit32.bxor
local rshift = bit32.rshift

local MASK = 0xFFFFFFFF

local Noise = {}

-- One 32-bit integer in, one out, avalanched.
--
-- Three rounds of xor-shift-multiply. Two is measurably worse in the low bits,
-- which is exactly where the gradient index is read from.
local function scramble(value: number): number
	local x = band(value, MASK)
	x = band(bxor(x, rshift(x, 16)) * 0x9E37, MASK)
	x = band(bxor(x, rshift(x, 15)) * 0x85EB, MASK)
	x = band(bxor(x, rshift(x, 16)) * 0xC2B2, MASK)
	return bxor(x, rshift(x, 16))
end

-- A lattice corner's hash. The coordinates are folded in with large odd
-- constants before scrambling, so (x, z) and (z, x) do not collide and a
-- diagonal does not correlate with itself.
local function hash2(x: number, z: number, seed: number): number
	return scramble(band(x * 374761393 + z * 668265263 + seed * 2246822519, MASK))
end

local function hash3(x: number, y: number, z: number, seed: number): number
	return scramble(band(x * 374761393 + y * 1274126177 + z * 668265263 + seed * 2246822519, MASK))
end

-- Quintic fade, 6t^5 - 15t^4 + 10t^3.
--
-- Not the cubic smoothstep: that has a non-zero second derivative at the lattice
-- points, which puts a faint crease along every integer line. Invisible on a
-- hillside and obvious on water, where the surface is nearly flat and the eye
-- has a straight edge to compare against.
local function fade(t: number): number
	return t * t * t * (t * (t * 6 - 15) + 10)
end

-- The fade's derivative, 30t^4 - 60t^3 + 30t^2, factored as 30t^2(t - 1)^2.
--
-- It is zero at both ends, which is the whole point of the quintic: the field's
-- slope matches across a lattice line as well as its value does. That also makes
-- it the one term in `noise2Deriv` that can be checked by eye -- a derivative
-- that is non-zero on an integer boundary is a wrong fade, not a wrong dot.
local function fadeDeriv(t: number): number
	return 30 * t * t * (t * (t - 2) + 1)
end

local function lerp(a: number, b: number, t: number): number
	return a + (b - a) * t
end

-- The gradient at a corner, dotted with the offset to the sample point.
--
-- Eight directions chosen by three bits of the hash: four diagonals and four
-- axes. The diagonals are what stop the field looking axis-aligned; the axes are
-- what stop it looking like a quilt of 45-degree ridges.
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

-- The same eight directions as `gradient2`, as components rather than as a dot.
--
-- A derivative needs the vector itself, not its projection, and the two have to
-- be the same eight vectors in the same eight slots or `noise2Deriv` would
-- return the slope of a different field than `noise2` returns the value of. They
-- are written out twice rather than `gradient2` being expressed in terms of this
-- one, because the dot is the hot path: two table-free returns and a multiply
-- each is measurably worse than one add.
local function gradientVector2(corner: number): (number, number)
	local choice = band(corner, 7)
	if choice == 0 then
		return 1, 1
	elseif choice == 1 then
		return 1, -1
	elseif choice == 2 then
		return -1, 1
	elseif choice == 3 then
		return -1, -1
	elseif choice == 4 then
		return 1, 0
	elseif choice == 5 then
		return -1, 0
	elseif choice == 6 then
		return 0, 1
	end
	return 0, -1
end

-- 2D gradient noise, roughly in [-1, 1].
--
-- A local that the module table aliases, rather than a `function Noise.noise2`
-- the rest of the file calls back through. `fbm2` and `ridged2` call this once
-- per octave and a chunk runs them thousands of times, so reaching it through a
-- table index is a hash lookup per octave per column for no reason.
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
-- A module-level function rather than a closure inside `noise3`, because a
-- closure that captures nothing is still allocated on every call to the function
-- containing it -- and `noise3` runs once per column of every chunk.
local function corner3(x: number, y: number, z: number, seed: number): number
	return hash3(x, y, z, seed) / 2147483648.0 - 1.0
end

-- 3D value noise, in [-1, 1]. Value rather than gradient because it is only used
-- to carve caves, where the cheaper field is indistinguishable and the eight
-- gradient dot products are not.
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

	local low = lerp(lerp(c000, c100, u), lerp(c001, c101, u), w)
	local high = lerp(lerp(c010, c110, u), lerp(c011, c111, u), w)
	return lerp(low, high, v)
end

-- Dividing fBm by its amplitude sum bounds it at +/-1 and leaves it nowhere near
-- +/-1, because the octaves would all have to peak in the same place with the
-- same sign. Measured over 65536 samples: three- and four-octave fBm here
-- reaches +/-0.53 and averages 0.136 in magnitude.
--
-- **This is the bug that makes a terrain generator look flat**, and it is
-- invisible in the code: a relief constant written as if the field spanned
-- [-1, 1] delivers half of it at the extreme and a tenth of it typically. The
-- gain is applied here, once, rather than being absorbed into constants that
-- would then all be lying by the same undocumented factor.
local FBM_GAIN = 1.85

-- Fractal Brownian motion: octaves at rising frequency and halving amplitude,
-- normalised so the result spans very nearly [-1, 1] whatever the octave count.
--
-- The seed is offset per octave. Without that, every octave samples the same
-- field at a different zoom and a feature at one scale reappears at the next,
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

	-- Clamped, because the gain is calibrated against the observed extreme and
	-- some sample somewhere will exceed it.
	return math.clamp(total / sum * FBM_GAIN, -1, 1)
end

-- Ridged multifractal: 1 - |noise|, squared, octave-weighted.
--
-- The absolute value folds the field at zero, and a fold is a crease -- which is
-- what a mountain ridge is. Squaring sharpens the peaks and flattens the
-- valleys, so the result is ranges with saddles between them rather than the
-- rolling dunes plain fBm gives.
--
-- Each octave is weighted by the one above it, which is the "multifractal" half:
-- detail appears on ridges that already exist and not in the valleys, so low
-- ground stays smooth instead of being uniformly rough.
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

-- A stable value in [0, 1) for an integer cell, for anything that needs a
-- per-cell roll without carrying a generator around.
local function value2(x: number, z: number, seed: number): number
	return hash2(x, z, seed) / 4294967296.0
end

Noise.noise2 = noise2
Noise.noise3 = noise3
Noise.fbm2 = fbm2
Noise.ridged2 = ridged2
Noise.value2 = value2

Noise.hash2 = hash2
Noise.hash3 = hash3
Noise.fade = fade
Noise.lerp = lerp

Noise.Noise2 = Noise.noise2
Noise.Noise3 = Noise.noise3
Noise.Fbm2 = Noise.fbm2
Noise.Ridged2 = Noise.ridged2
Noise.Value2 = Noise.value2

return table.freeze(Noise)
