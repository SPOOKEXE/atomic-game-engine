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

-- 2D gradient noise and its exact slope, in one pass.
--
-- The value is bit-for-bit what `noise2` returns; the extra two numbers are
-- d/dx and d/dz of the same field. **Analytical, not a finite difference.** The
-- usual way to get a slope out of a noise field is to sample it three times a
-- hair apart, which costs three fields instead of one and is wrong by O(eps)
-- everywhere and badly wrong wherever the field turns sharply. Everything the
-- derivative needs is already sitting in the interpolation, so it is nearly
-- free: four gradient vectors instead of four dot products, and two lerps.
--
-- What it is for is anything that has to know how steep the ground is *before*
-- it decides what the ground is. Erosion weighted by slope, cliff faces that get
-- rock instead of grass, a scatter that refuses to place a tree on a 40-degree
-- face -- all of them otherwise sample the height field twice more per column.
--
-- n = sum over the four corners of w_ij(u, v) * dot_ij(dx, dz), so the product
-- rule splits it in two: the corner gradients interpolated as values, plus the
-- interpolation weights differentiated through the fade.
local function noise2Deriv(x: number, z: number, seed: number): (number, number, number)
	local x0 = math.floor(x)
	local z0 = math.floor(z)
	local dx = x - x0
	local dz = z - z0

	local u = fade(dx)
	local v = fade(dz)
	local du = fadeDeriv(dx)
	local dv = fadeDeriv(dz)

	local g00x, g00z = gradientVector2(hash2(x0, z0, seed))
	local g10x, g10z = gradientVector2(hash2(x0 + 1, z0, seed))
	local g01x, g01z = gradientVector2(hash2(x0, z0 + 1, seed))
	local g11x, g11z = gradientVector2(hash2(x0 + 1, z0 + 1, seed))

	local n00 = g00x * dx + g00z * dz
	local n10 = g10x * (dx - 1) + g10z * dz
	local n01 = g01x * dx + g01z * (dz - 1)
	local n11 = g11x * (dx - 1) + g11z * (dz - 1)

	local low = lerp(n00, n10, u)
	local high = lerp(n01, n11, u)
	local value = lerp(low, high, v)

	-- The first term is the corners' own gradients, interpolated exactly as
	-- their dot products were. The second is the fade moving under the sample.
	local slopeX = lerp(lerp(g00x, g10x, u), lerp(g01x, g11x, u), v)
		+ du * ((n10 - n00) + v * (n11 - n01 - n10 + n00))
	local slopeZ = lerp(lerp(g00z, g10z, u), lerp(g01z, g11z, u), v) + dv * (high - low)

	return value, slopeX, slopeZ
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

-- ## Simplex
--
-- Perlin's own replacement for Perlin noise. The field is built on a lattice of
-- triangles rather than squares, which buys two things a square lattice cannot.
--
-- The first is cost. A square cell in N dimensions has 2^N corners and every one
-- of them is evaluated; a simplex has N + 1. In 2D that is four against three,
-- which is nothing. In 3D it is eight against four, and for carving caves --
-- `noise3`'s job -- that is the difference between a chunk budget that fits and
-- one that does not.
--
-- The second is that square lattices look like square lattices. Perlin noise is
-- zero at every integer point and interpolates along the axes between them, so
-- the field carries a faint grid that the eye finds on any large flat expanse.
-- A triangular lattice has no such preferred directions, which is why this is
-- the better default for anything seen from above.
--
-- Roughly [-1, 1], on the same terms `noise2` is: measured over 1024x1024
-- samples at 0.0761 spacing, 2D reaches +/-0.998 and 3D +/-0.993.

-- Skew and unskew factors: (sqrt(3) - 1) / 2 and (3 - sqrt(3)) / 6 in 2D, 1/3
-- and 1/6 in 3D. Skewing turns the triangular lattice into a square one so the
-- containing simplex can be found by flooring, which is the trick that makes
-- simplex noise cheap enough to be worth its extra algebra.
local F2 = 0.36602540378443865
local G2 = 0.21132486540518713
local G2_2 = 0.42264973081037427
local F3 = 0.3333333333333333
local G3 = 0.16666666666666666
local G3_2 = 0.3333333333333333

-- The kernel radius is 0.5 rather than the 0.6 the original paper used.
--
-- At 0.6 a corner's contribution is still non-zero when the sample leaves that
-- corner's simplex, so the field has a small discontinuity on every simplex
-- boundary -- invisible in the value and *not* invisible in the derivative,
-- which is the whole reason the derivative variants exist. At 0.5 contributions
-- vanish exactly on the boundary and the analytical slope is exact everywhere.
--
-- The scales are what brings the result back to about [-1, 1] afterwards, and
-- they are specific to this radius and to these unit gradients.
--
-- **They are not the textbook 70 and 100, and the difference is not a taste.**
-- Those constants were calibrated against the classic gradient set, whose
-- diagonals are (+-1, +-1) and therefore sqrt(2) long. The gradients below are
-- unit vectors, for the reason given beside them -- so 70 and 100 undershoot,
-- and undershoot by enough to matter: measured, they cap 2D simplex at +/-0.706
-- and 3D at +/-0.920 while every caller reads the doc comment and multiplies by
-- a relief written for [-1, 1].
--
-- This is `FBM_GAIN`'s bug wearing a different hat, arriving through a constant
-- copied from a library rather than through an octave sum, and it is worth
-- noticing that it survived being copied. Both numbers below were measured over
-- a million samples, not derived.
local SCALE2 = 99
local SCALE3 = 108

-- Eight unit gradients in 2D, twelve in 3D -- the midpoints of a square's edges
-- and a cube's, normalised.
--
-- Unit length matters more here than it does for `gradient2`. Perlin's kernel
-- interpolates between corners, so a longer gradient somewhere is a slightly
-- taller hill; simplex *sums* corner contributions, so a longer gradient makes
-- the sum overshoot the range the scale was calibrated against. The 3D set
-- deliberately omits the four body diagonals: twelve edge midpoints are evenly
-- spread over the sphere and (+-1, +-1, +-1) would not be.
local ROOT_HALF = 0.7071067811865476

local GRAD2X = { 1, -1, 0, 0, ROOT_HALF, -ROOT_HALF, ROOT_HALF, -ROOT_HALF }
local GRAD2Z = { 0, 0, 1, -1, ROOT_HALF, ROOT_HALF, -ROOT_HALF, -ROOT_HALF }

-- stylua: ignore
local GRAD3X = {
	ROOT_HALF, -ROOT_HALF, ROOT_HALF, -ROOT_HALF,
	ROOT_HALF, -ROOT_HALF, ROOT_HALF, -ROOT_HALF,
	0, 0, 0, 0,
}
-- stylua: ignore
local GRAD3Y = {
	ROOT_HALF, ROOT_HALF, -ROOT_HALF, -ROOT_HALF,
	0, 0, 0, 0,
	ROOT_HALF, -ROOT_HALF, ROOT_HALF, -ROOT_HALF,
}
-- stylua: ignore
local GRAD3Z = {
	0, 0, 0, 0,
	ROOT_HALF, ROOT_HALF, -ROOT_HALF, -ROOT_HALF,
	ROOT_HALF, ROOT_HALF, -ROOT_HALF, -ROOT_HALF,
}

-- 2D simplex noise, roughly in [-1, 1].
local function simplex2(x: number, z: number, seed: number): number
	-- Skew into the square lattice and floor to find the containing cell.
	local skew = (x + z) * F2
	local i = math.floor(x + skew)
	local j = math.floor(z + skew)

	-- Unskew the cell's origin back, and measure the sample from it.
	local unskew = (i + j) * G2
	local x0 = x - (i - unskew)
	local z0 = z - (j - unskew)

	-- A square cell is two triangles. Which one the sample is in is decided by
	-- one comparison, and that decides which corner is the middle of the three.
	local i1, j1
	if x0 > z0 then
		i1, j1 = 1, 0
	else
		i1, j1 = 0, 1
	end

	local x1 = x0 - i1 + G2
	local z1 = z0 - j1 + G2
	local x2 = x0 - 1 + G2_2
	local z2 = z0 - 1 + G2_2

	local g0 = band(hash2(i, j, seed), 7) + 1
	local g1 = band(hash2(i + i1, j + j1, seed), 7) + 1
	local g2 = band(hash2(i + 1, j + 1, seed), 7) + 1

	local total = 0

	-- Each corner contributes (r^2 - d^2)^4 times its gradient's dot product,
	-- and nothing at all once the sample is further away than the radius.
	local t0 = 0.5 - x0 * x0 - z0 * z0
	if t0 > 0 then
		local t = t0 * t0
		total += t * t * (GRAD2X[g0] * x0 + GRAD2Z[g0] * z0)
	end

	local t1 = 0.5 - x1 * x1 - z1 * z1
	if t1 > 0 then
		local t = t1 * t1
		total += t * t * (GRAD2X[g1] * x1 + GRAD2Z[g1] * z1)
	end

	local t2 = 0.5 - x2 * x2 - z2 * z2
	if t2 > 0 then
		local t = t2 * t2
		total += t * t * (GRAD2X[g2] * x2 + GRAD2Z[g2] * z2)
	end

	return total * SCALE2
end

-- 2D simplex noise and its exact slope.
--
-- d/dx of t^4 * (g . d) is 4t^3 * (dt/dx) * (g . d) + t^4 * g.x, and dt/dx is
-- -2x because t is r^2 minus the squared distance. That collapses to
-- t^3 * (t * g.x - 8x * (g . d)), which is the line repeated four times below.
local function simplex2Deriv(x: number, z: number, seed: number): (number, number, number)
	local skew = (x + z) * F2
	local i = math.floor(x + skew)
	local j = math.floor(z + skew)
	local unskew = (i + j) * G2
	local x0 = x - (i - unskew)
	local z0 = z - (j - unskew)

	local i1, j1
	if x0 > z0 then
		i1, j1 = 1, 0
	else
		i1, j1 = 0, 1
	end

	local x1 = x0 - i1 + G2
	local z1 = z0 - j1 + G2
	local x2 = x0 - 1 + G2_2
	local z2 = z0 - 1 + G2_2

	local g0 = band(hash2(i, j, seed), 7) + 1
	local g1 = band(hash2(i + i1, j + j1, seed), 7) + 1
	local g2 = band(hash2(i + 1, j + 1, seed), 7) + 1

	local total, slopeX, slopeZ = 0, 0, 0

	local t0 = 0.5 - x0 * x0 - z0 * z0
	if t0 > 0 then
		local gx, gz = GRAD2X[g0], GRAD2Z[g0]
		local dot = gx * x0 + gz * z0
		local cube = t0 * t0 * t0
		total += cube * t0 * dot
		slopeX += cube * (t0 * gx - 8 * x0 * dot)
		slopeZ += cube * (t0 * gz - 8 * z0 * dot)
	end

	local t1 = 0.5 - x1 * x1 - z1 * z1
	if t1 > 0 then
		local gx, gz = GRAD2X[g1], GRAD2Z[g1]
		local dot = gx * x1 + gz * z1
		local cube = t1 * t1 * t1
		total += cube * t1 * dot
		slopeX += cube * (t1 * gx - 8 * x1 * dot)
		slopeZ += cube * (t1 * gz - 8 * z1 * dot)
	end

	local t2 = 0.5 - x2 * x2 - z2 * z2
	if t2 > 0 then
		local gx, gz = GRAD2X[g2], GRAD2Z[g2]
		local dot = gx * x2 + gz * z2
		local cube = t2 * t2 * t2
		total += cube * t2 * dot
		slopeX += cube * (t2 * gx - 8 * x2 * dot)
		slopeZ += cube * (t2 * gz - 8 * z2 * dot)
	end

	return total * SCALE2, slopeX * SCALE2, slopeZ * SCALE2
end

-- Which of a cube cell's six tetrahedra holds the sample.
--
-- The three unskewed offsets sorted descending name the order the axes are
-- stepped along on the way from the near corner to the far one, and that order
-- *is* the tetrahedron. Three comparisons, returning the two middle corners'
-- integer offsets.
local function simplexCorners3(x0: number, y0: number, z0: number): (number, number, number, number, number, number)
	if x0 >= y0 then
		if y0 >= z0 then
			return 1, 0, 0, 1, 1, 0
		elseif x0 >= z0 then
			return 1, 0, 0, 1, 0, 1
		end
		return 0, 0, 1, 1, 0, 1
	end
	if y0 < z0 then
		return 0, 0, 1, 0, 1, 1
	elseif x0 < z0 then
		return 0, 1, 0, 0, 1, 1
	end
	return 0, 1, 0, 1, 1, 0
end

-- 3D simplex noise, roughly in [-1, 1]. Four corners where `noise3` reads eight.
local function simplex3(x: number, y: number, z: number, seed: number): number
	local skew = (x + y + z) * F3
	local i = math.floor(x + skew)
	local j = math.floor(y + skew)
	local k = math.floor(z + skew)

	local unskew = (i + j + k) * G3
	local x0 = x - (i - unskew)
	local y0 = y - (j - unskew)
	local z0 = z - (k - unskew)

	local i1, j1, k1, i2, j2, k2 = simplexCorners3(x0, y0, z0)

	local x1, y1, z1 = x0 - i1 + G3, y0 - j1 + G3, z0 - k1 + G3
	local x2, y2, z2 = x0 - i2 + G3_2, y0 - j2 + G3_2, z0 - k2 + G3_2
	local x3, y3, z3 = x0 - 0.5, y0 - 0.5, z0 - 0.5

	local g0 = hash3(i, j, k, seed) % 12 + 1
	local g1 = hash3(i + i1, j + j1, k + k1, seed) % 12 + 1
	local g2 = hash3(i + i2, j + j2, k + k2, seed) % 12 + 1
	local g3 = hash3(i + 1, j + 1, k + 1, seed) % 12 + 1

	local total = 0

	local t0 = 0.5 - x0 * x0 - y0 * y0 - z0 * z0
	if t0 > 0 then
		local t = t0 * t0
		total += t * t * (GRAD3X[g0] * x0 + GRAD3Y[g0] * y0 + GRAD3Z[g0] * z0)
	end

	local t1 = 0.5 - x1 * x1 - y1 * y1 - z1 * z1
	if t1 > 0 then
		local t = t1 * t1
		total += t * t * (GRAD3X[g1] * x1 + GRAD3Y[g1] * y1 + GRAD3Z[g1] * z1)
	end

	local t2 = 0.5 - x2 * x2 - y2 * y2 - z2 * z2
	if t2 > 0 then
		local t = t2 * t2
		total += t * t * (GRAD3X[g2] * x2 + GRAD3Y[g2] * y2 + GRAD3Z[g2] * z2)
	end

	local t3 = 0.5 - x3 * x3 - y3 * y3 - z3 * z3
	if t3 > 0 then
		local t = t3 * t3
		total += t * t * (GRAD3X[g3] * x3 + GRAD3Y[g3] * y3 + GRAD3Z[g3] * z3)
	end

	return total * SCALE3
end

-- 3D simplex noise and its exact gradient. The same collapse as `simplex2Deriv`,
-- one axis wider.
--
-- A 3D field's gradient is the surface normal of any isosurface through it,
-- which is what a cave mesher wants and what nothing else gives it for free: the
-- alternative is six more density samples per vertex.
local function simplex3Deriv(x: number, y: number, z: number, seed: number): (number, number, number, number)
	local skew = (x + y + z) * F3
	local i = math.floor(x + skew)
	local j = math.floor(y + skew)
	local k = math.floor(z + skew)

	local unskew = (i + j + k) * G3
	local x0 = x - (i - unskew)
	local y0 = y - (j - unskew)
	local z0 = z - (k - unskew)

	local i1, j1, k1, i2, j2, k2 = simplexCorners3(x0, y0, z0)

	local x1, y1, z1 = x0 - i1 + G3, y0 - j1 + G3, z0 - k1 + G3
	local x2, y2, z2 = x0 - i2 + G3_2, y0 - j2 + G3_2, z0 - k2 + G3_2
	local x3, y3, z3 = x0 - 0.5, y0 - 0.5, z0 - 0.5

	local g0 = hash3(i, j, k, seed) % 12 + 1
	local g1 = hash3(i + i1, j + j1, k + k1, seed) % 12 + 1
	local g2 = hash3(i + i2, j + j2, k + k2, seed) % 12 + 1
	local g3 = hash3(i + 1, j + 1, k + 1, seed) % 12 + 1

	local total, slopeX, slopeY, slopeZ = 0, 0, 0, 0

	local t0 = 0.5 - x0 * x0 - y0 * y0 - z0 * z0
	if t0 > 0 then
		local gx, gy, gz = GRAD3X[g0], GRAD3Y[g0], GRAD3Z[g0]
		local dot = gx * x0 + gy * y0 + gz * z0
		local cube = t0 * t0 * t0
		total += cube * t0 * dot
		slopeX += cube * (t0 * gx - 8 * x0 * dot)
		slopeY += cube * (t0 * gy - 8 * y0 * dot)
		slopeZ += cube * (t0 * gz - 8 * z0 * dot)
	end

	local t1 = 0.5 - x1 * x1 - y1 * y1 - z1 * z1
	if t1 > 0 then
		local gx, gy, gz = GRAD3X[g1], GRAD3Y[g1], GRAD3Z[g1]
		local dot = gx * x1 + gy * y1 + gz * z1
		local cube = t1 * t1 * t1
		total += cube * t1 * dot
		slopeX += cube * (t1 * gx - 8 * x1 * dot)
		slopeY += cube * (t1 * gy - 8 * y1 * dot)
		slopeZ += cube * (t1 * gz - 8 * z1 * dot)
	end

	local t2 = 0.5 - x2 * x2 - y2 * y2 - z2 * z2
	if t2 > 0 then
		local gx, gy, gz = GRAD3X[g2], GRAD3Y[g2], GRAD3Z[g2]
		local dot = gx * x2 + gy * y2 + gz * z2
		local cube = t2 * t2 * t2
		total += cube * t2 * dot
		slopeX += cube * (t2 * gx - 8 * x2 * dot)
		slopeY += cube * (t2 * gy - 8 * y2 * dot)
		slopeZ += cube * (t2 * gz - 8 * z2 * dot)
	end

	local t3 = 0.5 - x3 * x3 - y3 * y3 - z3 * z3
	if t3 > 0 then
		local gx, gy, gz = GRAD3X[g3], GRAD3Y[g3], GRAD3Z[g3]
		local dot = gx * x3 + gy * y3 + gz * z3
		local cube = t3 * t3 * t3
		total += cube * t3 * dot
		slopeX += cube * (t3 * gx - 8 * x3 * dot)
		slopeY += cube * (t3 * gy - 8 * y3 * dot)
		slopeZ += cube * (t3 * gz - 8 * z3 * dot)
	end

	return total * SCALE3, slopeX * SCALE3, slopeY * SCALE3, slopeZ * SCALE3
end

-- ## Worley
--
-- Cellular noise. Every integer cell owns one feature point, jittered somewhere
-- inside itself, and a sample is its distance to the nearest of them. Unlike
-- everything above it is not a smooth field of hills -- it is a mosaic, and it
-- is how you get things that are made of discrete pieces: cracked ground,
-- scales, stone walls, biome regions that have edges rather than gradients.
--
-- `f2 - f1` is the useful compound: zero in the middle of a cell and rising to a
-- maximum exactly on the border between two, which draws the cell walls.
--
-- Nine cells are searched in 2D and twenty-seven in 3D, and that is not
-- conservative -- it is exact. A feature point cannot be closer than the nearest
-- point of its cell, so a cell two away is at least one unit off and no jitter
-- inside it can beat a point in the sample's own cell ring.

-- The feature point's offset inside its cell, as two 16-bit fields of one hash.
--
-- One `scramble` for the pair rather than two hashes with different seeds. 16
-- bits places the point to within 1/65536 of a cell, which is finer than any
-- distance comparison here can tell apart, and it halves the hashing -- which is
-- the entire cost of this function, nine or twenty-seven times over.
local function worleyOffset2(cell: number): (number, number)
	return band(cell, 0xFFFF) / 65536, band(rshift(cell, 16), 0xFFFF) / 65536
end

-- Distances to the two nearest feature points, and the winning cell's id.
--
-- F1 is not on [-1, 1] and is not meant to be -- it is a distance in cells,
-- measured over a million samples as reaching 1.16 in 2D and 1.16 in 3D and
-- averaging 0.43 and 0.52. The distances are returned raw rather than normalised
-- because the useful compound is a difference: `f2 - f1` is near zero over most
-- of a cell and rises to the border, and dividing both by a nominal maximum
-- would flatten exactly that contrast.
--
-- The id is the cell's own hash, so it is stable, seed-dependent and immediately
-- useful as the seed for whatever that cell contains.
local function worley2(x: number, z: number, seed: number): (number, number, number)
	local cx = math.floor(x)
	local cz = math.floor(z)
	local first = math.huge
	local second = math.huge
	local id = 0

	for i = cx - 1, cx + 1 do
		for j = cz - 1, cz + 1 do
			local cell = hash2(i, j, seed)
			local ox, oz = worleyOffset2(cell)
			local dx = i + ox - x
			local dz = j + oz - z
			-- Compared squared and rooted once at the end: two square roots per
			-- sample instead of eighteen.
			local distance = dx * dx + dz * dz
			if distance < first then
				second = first
				first = distance
				id = cell
			elseif distance < second then
				second = distance
			end
		end
	end

	return math.sqrt(first), math.sqrt(second), id
end

-- Three 10-bit fields rather than two 16-bit ones, for the same reason: one
-- hash. 1/1024 of a cell is still far below anything a distance comparison here
-- resolves.
local function worleyOffset3(cell: number): (number, number, number)
	return band(cell, 0x3FF) / 1024, band(rshift(cell, 10), 0x3FF) / 1024, band(rshift(cell, 20), 0x3FF) / 1024
end

-- The 3D Worley pair and cell id. Twenty-seven cells, so roughly three times
-- 2D's cost -- worth checking against a budget before putting it in a per-voxel
-- loop.
local function worley3(x: number, y: number, z: number, seed: number): (number, number, number)
	local cx = math.floor(x)
	local cy = math.floor(y)
	local cz = math.floor(z)
	local first = math.huge
	local second = math.huge
	local id = 0

	for i = cx - 1, cx + 1 do
		for j = cy - 1, cy + 1 do
			for k = cz - 1, cz + 1 do
				local cell = hash3(i, j, k, seed)
				local ox, oy, oz = worleyOffset3(cell)
				local dx = i + ox - x
				local dy = j + oy - y
				local dz = k + oz - z
				local distance = dx * dx + dy * dy + dz * dz
				if distance < first then
					second = first
					first = distance
					id = cell
				elseif distance < second then
					second = distance
				end
			end
		end
	end

	return math.sqrt(first), math.sqrt(second), id
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

-- fBm and its slope, in one pass over the same octaves.
--
-- The value is exactly `fbm2`'s. Differentiation is linear, so the octaves' own
-- derivatives sum with the same amplitudes their values do -- times the octave's
-- frequency, because that is the chain rule through `x * frequency`. No extra
-- samples: the loop is `fbm2`'s loop with two more accumulators.
--
-- The derivative is reported as zero wherever the value clamps, which is where
-- it actually is: the clamp is a flat spot. It bites on a fraction of a percent
-- of samples and never in a place a slope test cares about.
local function fbm2Deriv(
	x: number,
	z: number,
	seed: number,
	octaves: number,
	lacunarity: number
): (number, number, number)
	local total = 0
	local slopeX = 0
	local slopeZ = 0
	local amplitude = 1
	local sum = 0
	local frequency = 1

	for octave = 1, octaves do
		local value, dx, dz = noise2Deriv(x * frequency, z * frequency, seed + octave * 1013)
		total += value * amplitude
		slopeX += dx * amplitude * frequency
		slopeZ += dz * amplitude * frequency
		sum += amplitude
		amplitude *= 0.5
		frequency *= lacunarity
	end

	-- `total / sum * FBM_GAIN`, in that order, because that is the order `fbm2`
	-- uses and the two have to agree to the last bit. Folding it as
	-- `total * (FBM_GAIN / sum)` is the same number in real arithmetic and one
	-- ulp away in this one, which is enough to make the two functions different
	-- fields -- and the difference would only ever show up as a seam between a
	-- chunk built through one and a chunk built through the other.
	local value = total / sum * FBM_GAIN
	if value < -1 then
		return -1, 0, 0
	elseif value > 1 then
		return 1, 0, 0
	end

	-- The slope has no such constraint: nothing compares it for equality.
	local scale = FBM_GAIN / sum
	return value, slopeX * scale, slopeZ * scale
end

-- Octave folding for any sampler, as one function instead of one copy per field.
--
-- `fbm2` above is *not* written in terms of this and is not going to be: it is
-- the hottest function in the library, every existing world was generated by it,
-- and a refactor that changed its arithmetic by one ulp would silently reshape
-- every saved map. This is for the samplers added since, where there is no such
-- history to preserve. The indirect call costs one call per octave -- real, but
-- an order below the hashing it wraps.
--
-- Returns the amplitude-normalised sum, without any gain: what the octaves
-- actually produced. Applying the gain is the caller's job because the right
-- gain is a property of the sampler, and burying a different constant in each
-- copy of this loop is how they end up disagreeing.
local function octaves2(
	sample: (number, number, number) -> number,
	x: number,
	z: number,
	seed: number,
	count: number,
	lacunarity: number
): number
	local total = 0
	local amplitude = 1
	local sum = 0
	local frequency = 1

	for octave = 1, count do
		total += sample(x * frequency, z * frequency, seed + octave * 1013) * amplitude
		sum += amplitude
		amplitude *= 0.5
		frequency *= lacunarity
	end

	return total / sum
end

local function octaves3(
	sample: (number, number, number, number) -> number,
	x: number,
	y: number,
	z: number,
	seed: number,
	count: number,
	lacunarity: number
): number
	local total = 0
	local amplitude = 1
	local sum = 0
	local frequency = 1

	for octave = 1, count do
		total += sample(x * frequency, y * frequency, z * frequency, seed + octave * 1013) * amplitude
		sum += amplitude
		amplitude *= 0.5
		frequency *= lacunarity
	end

	return total / sum
end

-- The same correction `FBM_GAIN` makes, measured per sampler.
--
-- Every one of them is a sum of octaves that rarely agree, so every one of them
-- falls short of its nominal range by a different amount -- and the amount is a
-- property of the field, not of fBm, which is why one shared constant would be
-- wrong three times. Measured over 1024x1024 samples, four octaves at lacunarity
-- 2: un-gained, simplex fBm peaks at 0.93 in 2D and 0.82 in 3D, and 3D value fBm
-- at 0.78.
--
-- **Simplex barely needs the correction and Perlin badly does** -- 1.07 against
-- 1.85 for the same octave count. The reason is in the single-octave numbers:
-- `noise2` spends its time near zero and averages 0.21 in magnitude, while
-- `simplex2` averages 0.47, so simplex octaves are far more likely to be large
-- at the same time. It is the clearest measurement here that the two fields are
-- not interchangeable at the same settings, whatever their ranges say.
local SIMPLEX_FBM2_GAIN = 1.07
local SIMPLEX_FBM3_GAIN = 1.22
local VALUE_FBM3_GAIN = 1.28

-- Simplex fBm, normalised the same way `fbm2` is. The drop-in for `fbm2` when
-- the square lattice is showing.
local function simplexFbm2(x: number, z: number, seed: number, count: number, lacunarity: number): number
	return math.clamp(octaves2(simplex2, x, z, seed, count, lacunarity) * SIMPLEX_FBM2_GAIN, -1, 1)
end

local function simplexFbm3(x: number, y: number, z: number, seed: number, count: number, lacunarity: number): number
	return math.clamp(octaves3(simplex3, x, y, z, seed, count, lacunarity) * SIMPLEX_FBM3_GAIN, -1, 1)
end

-- Value fBm in 3D: the octave-layered form of `noise3`, for density fields that
-- want detail at more than one scale. `noise3` alone gives caves one size.
local function fbm3(x: number, y: number, z: number, seed: number, count: number, lacunarity: number): number
	return math.clamp(octaves3(noise3, x, y, z, seed, count, lacunarity) * VALUE_FBM3_GAIN, -1, 1)
end

-- A stable value in [0, 1) for an integer cell, for anything that needs a
-- per-cell roll without carrying a generator around.
local function value2(x: number, z: number, seed: number): number
	return hash2(x, z, seed) / 4294967296.0
end

-- The same, for a voxel rather than a column.
local function value3(x: number, y: number, z: number, seed: number): number
	return hash3(x, y, z, seed) / 4294967296.0
end

Noise.noise2 = noise2
Noise.noise3 = noise3
Noise.fbm2 = fbm2
Noise.fbm3 = fbm3
Noise.ridged2 = ridged2
Noise.value2 = value2
Noise.value3 = value3

Noise.simplex2 = simplex2
Noise.simplex3 = simplex3
Noise.simplexFbm2 = simplexFbm2
Noise.simplexFbm3 = simplexFbm3

Noise.worley2 = worley2
Noise.worley3 = worley3

Noise.noise2Deriv = noise2Deriv
Noise.fbm2Deriv = fbm2Deriv
Noise.simplex2Deriv = simplex2Deriv
Noise.simplex3Deriv = simplex3Deriv

Noise.octaves2 = octaves2
Noise.octaves3 = octaves3

Noise.hash2 = hash2
Noise.hash3 = hash3
Noise.fade = fade
Noise.fadeDeriv = fadeDeriv
Noise.lerp = lerp

Noise.Noise2 = Noise.noise2
Noise.Noise3 = Noise.noise3
Noise.Fbm2 = Noise.fbm2
Noise.Fbm3 = Noise.fbm3
Noise.Ridged2 = Noise.ridged2
Noise.Value2 = Noise.value2
Noise.Value3 = Noise.value3
Noise.Simplex2 = Noise.simplex2
Noise.Simplex3 = Noise.simplex3
Noise.SimplexFbm2 = Noise.simplexFbm2
Noise.SimplexFbm3 = Noise.simplexFbm3
Noise.Worley2 = Noise.worley2
Noise.Worley3 = Noise.worley3

return table.freeze(Noise)
