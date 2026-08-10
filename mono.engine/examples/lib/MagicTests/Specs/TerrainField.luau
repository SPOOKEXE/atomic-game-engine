--!strict

-- `TerrainCore`: the noise, the height field, the crater edits, and the mesher.
--
-- Every one of these runs without a Workspace, a Part or a place file, which is
-- the whole reason `TerrainCore` and `TerrainRuntime` are separate modules. A
-- generator that can only be checked by looking at it is a generator nobody
-- checks.
--
-- What is asserted here is the set of properties that are invisible on screen
-- until they are badly wrong:
--
--   * the noise is a pure function of its arguments, so two machines build the
--     same world;
--   * the field has real relief and a real coastline, rather than being the flat
--     plane an un-normalised fBm produces;
--   * neighbouring chunks agree along their shared edge;
--   * a merged box covers exactly the columns it replaced;
--   * a crater lowers the ground it covers and raises the rim outside it, and
--     survives a serialise/rebuild round trip.

return function(harness: any, _core: any, _fixtures: any, terrain: any)
	if terrain == nil then
		-- Skipped rather than failed when the terrain modules are absent, so a
		-- place that only installed MagicCore still runs the magic suite. The
		-- runner reports the skip.
		return
	end

	local Noise = terrain.Noise
	local NoiseMap = terrain.NoiseMap
	local HeightField = terrain.HeightField
	local Edits = terrain.Edits
	local Chunker = terrain.Chunker

	local SEED = 1234
	local CHUNK = 32

	harness:suite("terrain noise", function(h: any)
		h:test("is a pure function of its arguments", function()
			for index = 1, 32 do
				local x = index * 3.25
				local z = index * -7.5
				h:assertEqual(Noise.noise2(x, z, SEED), Noise.noise2(x, z, SEED))
				h:assertEqual(Noise.fbm2(x, z, SEED, 4, 2), Noise.fbm2(x, z, SEED, 4, 2))
			end
		end)

		h:test("gradient noise is exactly zero on the lattice, whatever the seed", function()
			-- Not a quirk: at an integer point the offset to every corner is zero
			-- on the axis it shares, so the dot products vanish. Worth pinning,
			-- because it makes any test that samples on integers blind to the
			-- seed -- which is a mistake that looks like a broken hash.
			for index = -8, 8 do
				h:assertEqual(Noise.noise2(index, index, SEED), 0)
				h:assertEqual(Noise.noise2(index, index, SEED + 991), 0)
			end
		end)

		h:test("a different seed is a different field between the lattice points", function()
			local same = 0
			for index = 1, 64 do
				local x = index + 0.37
				local z = index * -1.5 + 0.11
				if Noise.noise2(x, z, SEED) == Noise.noise2(x, z, SEED + 991) then
					same += 1
				end
			end
			h:assertTrue(same < 4, "two seeds produced the same field")
		end)

		h:test("gradient noise stays inside its nominal range", function()
			local low, high = math.huge, -math.huge
			for z = 0, 40 do
				for x = 0, 40 do
					local value = Noise.noise2(x * 0.37, z * 0.37, SEED)
					low = math.min(low, value)
					high = math.max(high, value)
				end
			end
			h:assertTrue(low >= -1.001 and high <= 1.001, "noise2 left [-1, 1]")
			-- And it actually uses the range: a field that never leaves the
			-- middle is the flat-terrain bug, caught here rather than by eye.
			h:assertTrue(high - low > 1, "noise2 barely varied")
		end)

		h:test("fbm reaches most of its range after the gain", function()
			-- The measured reason `FBM_GAIN` exists. Un-normalised, fBm peaks at
			-- about +/-0.53, so every relief constant written against [-1, 1]
			-- silently delivers half of it.
			local high = 0
			for z = 0, 120 do
				for x = 0, 120 do
					high = math.max(high, math.abs(Noise.fbm2(x * 0.11, z * 0.11, SEED, 4, 2)))
				end
			end
			h:assertTrue(high > 0.7, "fbm2 never approaches its nominal range")
			h:assertTrue(high <= 1.0000001, "fbm2 exceeded its clamp")
		end)

		h:test("the hash has no short period", function()
			-- A permutation-table Perlin repeats every 256 units, which on a real
			-- map is a visible grid of identical hills.
			local repeats = 0
			for index = 0, 200 do
				if Noise.hash2(index, 0, SEED) == Noise.hash2(index + 256, 0, SEED) then
					repeats += 1
				end
			end
			h:assertEqual(repeats, 0)
		end)
	end)

	-- Simplex and Worley came from Atlas, whose samplers are built on a shuffled
	-- 256-entry permutation table. The kernels were kept and that hashing was
	-- not, so the properties asserted here are the ones the substitution had to
	-- preserve and the one it exists to fix.
	harness:suite("simplex and worley noise", function(h: any)
		h:test("simplex is a pure function of its arguments", function()
			for index = 1, 32 do
				local x = index * 3.25
				local z = index * -7.5
				local y = index * 1.75
				h:assertEqual(Noise.simplex2(x, z, SEED), Noise.simplex2(x, z, SEED))
				h:assertEqual(Noise.simplex3(x, y, z, SEED), Noise.simplex3(x, y, z, SEED))
			end
		end)

		h:test("simplex has no 256-unit period either", function()
			-- The whole reason the ported kernels are hashed with `hash2` rather
			-- than with the permutation table they arrived with. A table-based
			-- simplex matches itself exactly at every offset of 256.
			local repeats = 0
			for index = 0, 200 do
				if Noise.simplex2(index + 0.31, 0.17, SEED) == Noise.simplex2(index + 256.31, 0.17, SEED) then
					repeats += 1
				end
			end
			h:assertEqual(repeats, 0)
		end)

		h:test("simplex reaches most of its nominal range", function()
			-- The flat-terrain bug, caught in the constant rather than in the
			-- octave sum: the textbook scale of 70 was calibrated for gradients
			-- sqrt(2) long and caps this field at 0.71 with unit ones.
			local low, high = math.huge, -math.huge
			for z = 0, 120 do
				for x = 0, 120 do
					local value = Noise.simplex2(x * 0.37, z * 0.37, SEED)
					low = math.min(low, value)
					high = math.max(high, value)
				end
			end
			h:assertTrue(low >= -1.001 and high <= 1.001, "simplex2 left [-1, 1]")
			h:assertTrue(high > 0.9 and low < -0.9, "simplex2 does not use its range")
		end)

		h:test("3D simplex reaches most of its nominal range", function()
			local low, high = math.huge, -math.huge
			for index = 0, 20000 do
				local value = Noise.simplex3(index * 0.13, index * -0.07, index * 0.31, SEED)
				low = math.min(low, value)
				high = math.max(high, value)
			end
			h:assertTrue(low >= -1.001 and high <= 1.001, "simplex3 left [-1, 1]")
			h:assertTrue(high > 0.85 and low < -0.85, "simplex3 does not use its range")
		end)

		h:test("simplex is not zero on the lattice, where gradient noise is", function()
			-- Not a curiosity: it is the reason simplex has no visible grid. A
			-- square-lattice field is pinned to zero at every integer point and
			-- the eye finds the pattern; a triangular one is not pinned anywhere.
			local nonZero = 0
			for index = -8, 8 do
				if Noise.simplex2(index, index, SEED) ~= 0 then
					nonZero += 1
				end
			end
			h:assertTrue(nonZero > 12, "simplex2 is behaving like a square lattice")
		end)

		h:test("simplex fbm stays inside its range and reaches most of it", function()
			local low, high = math.huge, -math.huge
			for z = 0, 120 do
				for x = 0, 120 do
					local value = Noise.simplexFbm2(x * 0.11, z * 0.11, SEED, 4, 2)
					low = math.min(low, value)
					high = math.max(high, value)
				end
			end
			h:assertTrue(low >= -1.0000001 and high <= 1.0000001, "simplexFbm2 exceeded its clamp")
			h:assertTrue(high > 0.7 and low < -0.7, "simplexFbm2 never approaches its range")
		end)

		h:test("worley returns its two distances in order, with a stable cell id", function()
			for index = 1, 400 do
				local x = index * 0.317
				local z = index * -0.211
				local first, second, id = Noise.worley2(x, z, SEED)
				h:assertTrue(first >= 0, "F1 is negative")
				h:assertTrue(second >= first, "F2 is nearer than F1")
				local againFirst, againSecond, againId = Noise.worley2(x, z, SEED)
				h:assertEqual(againFirst, first)
				h:assertEqual(againSecond, second)
				h:assertEqual(againId, id)
			end
		end)

		h:test("worley in 3D orders its distances too", function()
			for index = 1, 200 do
				local first, second = Noise.worley3(index * 0.317, index * 0.113, index * -0.211, SEED)
				h:assertTrue(first >= 0 and second >= first, "the 3D pair is out of order")
			end
		end)

		h:test("the worley cell id is a real cell id, not a coordinate hash", function()
			-- Two samples a fraction of a cell apart are almost always inside the
			-- same cell and must name the same one. An id derived from the sample
			-- point rather than from the winning cell would differ every time,
			-- which is invisible until per-cell content starts flickering.
			local same = 0
			for index = 1, 200 do
				local x = index * 3 + 0.5
				local z = index * -2 + 0.5
				local _, _, here = Noise.worley2(x, z, SEED)
				local _, _, nearby = Noise.worley2(x + 0.01, z + 0.01, SEED)
				if here == nearby then
					same += 1
				end
			end
			h:assertTrue(same > 190, "the cell id changes within a cell")
		end)

		h:test("worley draws borders: f2 - f1 falls to nothing inside a cell", function()
			local low, high = math.huge, -math.huge
			for z = 0, 80 do
				for x = 0, 80 do
					local first, second = Noise.worley2(x * 0.19, z * 0.19, SEED)
					local edge = second - first
					low = math.min(low, edge)
					high = math.max(high, edge)
				end
			end
			h:assertTrue(low < 0.05, "no sample landed near a cell border")
			h:assertTrue(high > 0.5, "the cell interiors have no contrast")
		end)
	end)

	-- Analytical derivatives, checked the way the code says not to compute them:
	-- against central differences. A finite difference is a poor slope and a
	-- perfectly good oracle, because the two are wrong in unrelated ways.
	harness:suite("noise derivatives", function(h: any)
		local EPS = 1e-4
		local TOLERANCE = 1e-3

		h:test("the fade's derivative vanishes at both ends of a cell", function()
			-- What makes the quintic worth its extra terms: slope matches across
			-- a lattice line as well as value does. A non-zero value here is a
			-- crease along every integer line, and it is the one term in the
			-- derivative that can be checked without an oracle.
			h:assertEqual(Noise.fadeDeriv(0), 0)
			h:assertEqual(Noise.fadeDeriv(1), 0)
			h:assertTrue(Noise.fadeDeriv(0.5) > 1.8, "the fade barely moves in the middle")
		end)

		h:test("gradient noise reports its own value alongside its slope", function()
			-- Exactly equal, not nearly: `noise2Deriv` recomputes the same dot
			-- products in the same order. A drift here means the two functions
			-- have quietly become two different fields.
			for index = 1, 200 do
				local x = index * 0.137 - 10
				local z = index * -0.231 + 6
				local value = Noise.noise2Deriv(x, z, SEED)
				h:assertEqual(value, Noise.noise2(x, z, SEED))
			end
		end)

		h:test("gradient noise's slope matches a central difference", function()
			for index = 1, 200 do
				local x = index * 0.137 - 10
				local z = index * -0.231 + 6
				local _, slopeX, slopeZ = Noise.noise2Deriv(x, z, SEED)
				local byDifferenceX = (Noise.noise2(x + EPS, z, SEED) - Noise.noise2(x - EPS, z, SEED)) / (2 * EPS)
				local byDifferenceZ = (Noise.noise2(x, z + EPS, SEED) - Noise.noise2(x, z - EPS, SEED)) / (2 * EPS)
				h:assertNear(slopeX, byDifferenceX, TOLERANCE, "d/dx disagrees with the field")
				h:assertNear(slopeZ, byDifferenceZ, TOLERANCE, "d/dz disagrees with the field")
			end
		end)

		h:test("simplex reports its own value alongside its slope", function()
			for index = 1, 200 do
				local x = index * 0.137 - 10
				local z = index * -0.231 + 6
				local value = Noise.simplex2Deriv(x, z, SEED)
				h:assertNear(value, Noise.simplex2(x, z, SEED), 1e-12)
			end
		end)

		h:test("simplex's slope matches a central difference", function()
			-- The kernel radius is 0.5 rather than the paper's 0.6 precisely so
			-- this holds: at 0.6 a corner still contributes outside its own
			-- simplex and the field has a step in its slope on every boundary,
			-- which is invisible in the value and fails here.
			for index = 1, 200 do
				local x = index * 0.137 - 10
				local z = index * -0.231 + 6
				local _, slopeX, slopeZ = Noise.simplex2Deriv(x, z, SEED)
				local byDifferenceX = (Noise.simplex2(x + EPS, z, SEED) - Noise.simplex2(x - EPS, z, SEED)) / (2 * EPS)
				local byDifferenceZ = (Noise.simplex2(x, z + EPS, SEED) - Noise.simplex2(x, z - EPS, SEED)) / (2 * EPS)
				h:assertNear(slopeX, byDifferenceX, TOLERANCE, "simplex d/dx disagrees with the field")
				h:assertNear(slopeZ, byDifferenceZ, TOLERANCE, "simplex d/dz disagrees with the field")
			end
		end)

		h:test("3D simplex's gradient matches central differences on all three axes", function()
			for index = 1, 150 do
				local x = index * 0.137 - 10
				local y = index * 0.071 + 3
				local z = index * -0.231 + 6
				local value, slopeX, slopeY, slopeZ = Noise.simplex3Deriv(x, y, z, SEED)
				h:assertNear(value, Noise.simplex3(x, y, z, SEED), 1e-12)
				local byX = (Noise.simplex3(x + EPS, y, z, SEED) - Noise.simplex3(x - EPS, y, z, SEED)) / (2 * EPS)
				local byY = (Noise.simplex3(x, y + EPS, z, SEED) - Noise.simplex3(x, y - EPS, z, SEED)) / (2 * EPS)
				local byZ = (Noise.simplex3(x, y, z + EPS, SEED) - Noise.simplex3(x, y, z - EPS, SEED)) / (2 * EPS)
				h:assertNear(slopeX, byX, TOLERANCE, "simplex3 d/dx disagrees")
				h:assertNear(slopeY, byY, TOLERANCE, "simplex3 d/dy disagrees")
				h:assertNear(slopeZ, byZ, TOLERANCE, "simplex3 d/dz disagrees")
			end
		end)

		h:test("fbm reports the same value with a slope as without", function()
			for index = 1, 200 do
				local x = index * 0.137 - 10
				local z = index * -0.231 + 6
				local value = Noise.fbm2Deriv(x, z, SEED, 4, 2)
				h:assertEqual(value, Noise.fbm2(x, z, SEED, 4, 2))
			end
		end)

		h:test("fbm's slope is the octaves' slopes, chain-ruled by frequency", function()
			-- The property that makes it free: differentiation is linear, so the
			-- same loop accumulates both. Dropping the frequency factor gives a
			-- slope that is right at octave one and wrong by 2^n after it, which
			-- reads as terrain that is smooth to the eye and steep to the code.
			local checked = 0
			for index = 1, 200 do
				local x = index * 0.137 - 10
				local z = index * -0.231 + 6
				local value, slopeX, slopeZ = Noise.fbm2Deriv(x, z, SEED, 4, 2)
				-- Skipped where the clamp bites, which is a flat spot with a
				-- genuine slope of zero and a finite difference that disagrees.
				if math.abs(value) < 0.999 then
					local byX = (Noise.fbm2(x + EPS, z, SEED, 4, 2) - Noise.fbm2(x - EPS, z, SEED, 4, 2)) / (2 * EPS)
					local byZ = (Noise.fbm2(x, z + EPS, SEED, 4, 2) - Noise.fbm2(x, z - EPS, SEED, 4, 2)) / (2 * EPS)
					h:assertNear(slopeX, byX, TOLERANCE, "fbm d/dx disagrees with the field")
					h:assertNear(slopeZ, byZ, TOLERANCE, "fbm d/dz disagrees with the field")
					checked += 1
				end
			end
			h:assertTrue(checked > 190, "the clamp swallowed the test")
		end)
	end)

	harness:suite("noise maps", function(h: any)
		local function build(kind: string, width: number, depth: number): any
			return NoiseMap.build2({
				width = width,
				depth = depth,
				step = 4,
				originX = -128,
				originZ = 64,
				sample = NoiseMap.field2({ kind = kind, seed = SEED, scale = 400, octaves = 5 }),
			})
		end

		h:test("a map is reproducible from its spec", function()
			local first = build("simplexFbm", 24, 16)
			local second = build("simplexFbm", 24, 16)
			h:assertEqual(first.min, second.min)
			h:assertEqual(first.max, second.max)
			h:assertDeepEqual(first.values, second.values)
		end)

		h:test("a map records the range it actually sampled", function()
			-- Not the range its field nominally spans. Everything downstream --
			-- the normalise, the text ramp, the histogram -- stretches against
			-- these two numbers, so a stale bound is a washed-out picture.
			local map = build("fbm", 32, 20)
			h:assertEqual(#map.values, 32 * 20)
			local low, high = math.huge, -math.huge
			for _, value in map.values do
				low = math.min(low, value)
				high = math.max(high, value)
			end
			h:assertEqual(map.min, low)
			h:assertEqual(map.max, high)
		end)

		h:test("cells outside the map read as absent rather than throwing", function()
			local map = build("simplex", 8, 6)
			h:assertNil(NoiseMap.at2(map, 0, 1))
			h:assertNil(NoiseMap.at2(map, 9, 1))
			h:assertNil(NoiseMap.at2(map, 1, 0))
			h:assertNil(NoiseMap.at2(map, 1, 7))
			h:assertTrue(NoiseMap.at2(map, 8, 6) ~= nil)
		end)

		h:test("reading a map at a grid point returns that grid point exactly", function()
			-- The interpolation's fixed points. An off-by-one in the row stride
			-- shows up here and nowhere else, because a map read half a cell out
			-- is still a plausible-looking map.
			local map = build("simplexFbm", 24, 16)
			for row = 1, map.depth do
				for column = 1, map.width do
					local x = map.originX + (column - 1) * map.step
					local z = map.originZ + (row - 1) * map.step
					h:assertEqual(NoiseMap.sample2(map, x, z), NoiseMap.at2(map, column, row))
				end
			end
		end)

		h:test("reading outside a map clamps to its border", function()
			-- Rather than wrapping, which would look correct until two windows
			-- are placed side by side.
			local map = build("simplexFbm", 24, 16)
			h:assertEqual(NoiseMap.sample2(map, -1e6, -1e6), NoiseMap.at2(map, 1, 1))
			h:assertEqual(NoiseMap.sample2(map, 1e6, 1e6), NoiseMap.at2(map, map.width, map.depth))
		end)

		h:test("normalising stretches a map onto [0, 1] using its measured extremes", function()
			local map = build("fbm", 24, 16)
			local unit = NoiseMap.normalized2(map)
			h:assertEqual(unit.min, 0)
			h:assertEqual(unit.max, 1)
			local low, high = math.huge, -math.huge
			for _, value in unit.values do
				low = math.min(low, value)
				high = math.max(high, value)
			end
			h:assertNear(low, 0, 1e-12)
			h:assertNear(high, 1, 1e-12)
		end)

		h:test("a constant map normalises flat instead of dividing by nothing", function()
			local flat = NoiseMap.build2({ width = 4, depth = 4, sample = function() return 3 end })
			local unit = NoiseMap.normalized2(flat)
			h:assertEqual(unit.max, 0)
			for _, value in unit.values do
				h:assertEqual(value, 0)
			end
		end)

		h:test("remapping remeasures the range rather than transforming it", function()
			-- Squaring is non-monotonic across zero, so the old extremes are not
			-- the new ones and carrying them forward would break every reader.
			local map = build("fbm", 16, 12)
			local squared = NoiseMap.remapped2(map, function(value)
				return value * value
			end)
			h:assertTrue(squared.min >= 0, "a squared map has a negative minimum")
			for index, value in squared.values do
				h:assertNear(value, map.values[index] * map.values[index], 1e-12)
			end
		end)

		h:test("a 3D map is a stack of 2D ones and a slice is the layer it names", function()
			local volume = NoiseMap.build3({
				width = 8,
				height = 4,
				depth = 6,
				step = 2,
				sample = NoiseMap.field3({ kind = "simplex", seed = SEED, scale = 20 }),
			})
			h:assertEqual(#volume.values, 8 * 4 * 6)

			local slice = NoiseMap.slice3(volume, 3)
			h:assertEqual(slice.width, 8)
			h:assertEqual(slice.depth, 6)
			for row = 1, 6 do
				for column = 1, 8 do
					h:assertEqual(NoiseMap.at2(slice, column, row), NoiseMap.at3(volume, column, row, 3))
				end
			end
		end)

		h:test("reading a 3D map at a grid point returns that grid point exactly", function()
			local volume = NoiseMap.build3({
				width = 8,
				height = 4,
				depth = 6,
				step = 2,
				sample = NoiseMap.field3({ kind = "simplexFbm", seed = SEED, scale = 30 }),
			})
			for layer = 1, 4 do
				for row = 1, 6 do
					for column = 1, 8 do
						local x = (column - 1) * 2
						local y = (layer - 1) * 2
						local z = (row - 1) * 2
						h:assertEqual(NoiseMap.sample3(volume, x, y, z), NoiseMap.at3(volume, column, row, layer))
					end
				end
			end
		end)

		h:test("a histogram counts every cell and spreads a real field across buckets", function()
			-- The check a range test cannot make: a field can span its whole
			-- range and still spend almost all of its samples in the middle of
			-- it, which on screen is a flat plain with two spikes.
			local map = NoiseMap.normalized2(build("simplexFbm", 48, 32))
			local counts = NoiseMap.histogram2(map, 8)
			local total, occupied = 0, 0
			for _, count in counts do
				total += count
				if count > 0 then
					occupied += 1
				end
			end
			h:assertEqual(total, 48 * 32)
			h:assertTrue(occupied >= 6, "the field is stuck in two buckets")
		end)

		h:test("the text rendering is one line per row and one glyph per cell", function()
			local map = build("worleyEdge", 20, 7)
			local lines = string.split(NoiseMap.toText2(map), "\n")
			h:assertEqual(#lines, 7)
			for _, line in lines do
				h:assertEqual(#line, 20)
			end
		end)

		h:test("an unknown field kind is refused rather than silently flat", function()
			h:assertThrows(function()
				NoiseMap.field2({ kind = "perlinn", seed = SEED })
			end, "unknown kind")
			h:assertThrows(function()
				NoiseMap.field3({ kind = "ridged", seed = SEED })
			end, "unknown kind")
		end)

		h:test("a map's dimensions must be whole and positive", function()
			h:assertThrows(function()
				NoiseMap.build2({ width = 0, depth = 4, sample = function() return 0 end })
			end, "positive integer")
			h:assertThrows(function()
				NoiseMap.build2({ width = 4.5, depth = 4, sample = function() return 0 end })
			end, "positive integer")
		end)
	end)

	harness:suite("height field", function(h: any)
		local field = HeightField.new({ seed = SEED })

		h:test("has land, sea and real relief", function()
			local low, high, wet, total = math.huge, -math.huge, 0, 0
			for z = -4000, 4000, 211 do
				for x = -4000, 4000, 211 do
					local height = field:heightAt(x, z)
					low = math.min(low, height)
					high = math.max(high, height)
					if height < field.settings.seaLevel then
						wet += 1
					end
					total += 1
				end
			end
			h:assertTrue(high - low > 50, "the world is flat")
			h:assertTrue(wet > total * 0.1, "there is no sea")
			h:assertTrue(wet < total * 0.9, "there is no land")
		end)

		h:test("is a pure function, so two builds agree", function()
			local other = HeightField.new({ seed = SEED })
			for index = -20, 20 do
				h:assertEqual(field:heightAt(index * 13, index * -7), other:heightAt(index * 13, index * -7))
			end
		end)

		h:test("neighbouring columns never jump like a seam", function()
			-- The coarse lattice is chunk-aligned so two chunks interpolate
			-- between the same samples. A misaligned lattice shows up as a step
			-- exactly on a chunk boundary and nowhere else.
			local worst = 0
			for z = -64, 64 do
				local left = field:heightAt(-1, z)
				local right = field:heightAt(0, z)
				worst = math.max(worst, math.abs(left - right))
			end
			h:assertTrue(worst < 6, "a chunk boundary has a step in it")
		end)

		h:test("a surface is chosen for every column and water only below sea level", function()
			for index = -60, 60 do
				local x = index * 17
				local z = index * -23
				local top = math.floor(field:heightAt(x, z))
				local warmth, humidity = field:climateAt(x, z)
				local surface = field:surfaceAt(top, 0, warmth, humidity, 0)
				h:assertTrue(surface >= 1 and surface <= #HeightField.Surfaces)
				if surface == HeightField.SurfaceWater then
					h:assertTrue(top < field.settings.seaLevel)
				end
				-- Scorch is an edit's material and must never appear naturally.
				h:assertTrue(surface ~= HeightField.SurfaceScorch)
			end
		end)
	end)

	harness:suite("chunk meshing", function(h: any)
		local field = HeightField.new({ seed = SEED })

		h:test("merges far fewer boxes than there are columns", function()
			local boxes, columns = 0, 0
			for cz = -1, 1 do
				for cx = -1, 1 do
					boxes += Chunker.build(field, cx, cz, CHUNK).count
					columns += CHUNK * CHUNK
				end
			end
			h:assertTrue(boxes < columns * 0.5, "the merge is not merging")
			h:assertTrue(boxes > 0, "the mesher produced nothing")
		end)

		h:test("every box stays inside its own chunk", function()
			-- A run that ran past the chunk edge would overlap the neighbour and
			-- z-fight along the seam.
			local chunk = Chunker.build(field, 2, -3, CHUNK)
			local minX = 2 * CHUNK
			local minZ = -3 * CHUNK
			for _, box in chunk.boxes do
				h:assertTrue(box.x0 >= minX and box.x1 < minX + CHUNK)
				h:assertTrue(box.z0 >= minZ and box.z1 < minZ + CHUNK)
				h:assertTrue(box.x1 >= box.x0 and box.z1 >= box.z0 and box.top >= box.bottom)
			end
		end)

		h:test("the top of every ground column is covered by exactly one box", function()
			-- The property the merge has to preserve: a merged box is the voxels
			-- it replaced, so the surface is still fully covered and never twice.
			local chunk = Chunker.build(field, 0, 0, CHUNK)
			local covering: { [string]: number } = {}
			for _, box in chunk.boxes do
				if box.surface == HeightField.SurfaceWater then
					continue
				end
				for x = box.x0, box.x1 do
					for z = box.z0, box.z1 do
						local key = `{x}:{z}`
						covering[key] = math.max(covering[key] or -math.huge, box.top)
					end
				end
			end

			for x = 0, CHUNK - 1 do
				for z = 0, CHUNK - 1 do
					local expected = field:solidTopAt(x, z, field:heightAt(x, z))
					if expected >= field.settings.seaLevel then
						h:assertEqual(covering[`{x}:{z}`], expected, `column {x},{z} is not covered by its own top`)
					end
				end
			end
		end)

		h:test("box geometry converts inclusive bounds to a centre and a size", function()
			-- A single block at index 4 occupies [4, 5) and is centred at 4.5.
			-- Getting this off by half shifts the whole world half a stud, which
			-- is only visible at a chunk seam.
			local x, y, z, width, height, depth = Chunker.boxGeometry({
				x0 = 4, x1 = 4, z0 = -2, z1 = 1, bottom = 10, top = 12, surface = 1,
			})
			h:assertEqual(width, 1)
			h:assertEqual(depth, 4)
			h:assertEqual(height, 3)
			h:assertNear(x, 4.5)
			h:assertNear(z, 0)
			h:assertNear(y, 11.5)
		end)

		h:test("a chunk is the same every time it is built", function()
			local first = Chunker.build(field, -5, 7, CHUNK)
			local second = Chunker.build(field, -5, 7, CHUNK)
			h:assertEqual(first.count, second.count)
			for index = 1, first.count do
				h:assertDeepEqual(first.boxes[index], second.boxes[index])
			end
		end)
	end)

	harness:suite("terrain edits", function(h: any)
		h:test("an empty field changes nothing", function()
			local edits = Edits.new()
			h:assertEqual(edits:count(), 0)
			h:assertEqual(edits:deltaAt(0, 0), 0)
			h:assertEqual(edits:deltaAt(9999, -9999), 0)
		end)

		h:test("a crater digs at its centre and raises a rim outside it", function()
			local edits = Edits.new()
			local crater = edits:crater(0, 0, 20, 10, 7)

			h:assertTrue(edits:deltaAt(0, 0) < -8, "the centre did not sink")
			-- Just past the lip, ejecta stands above the untouched ground.
			local rim = edits:deltaAt(crater.radius * 1.12, 0)
			h:assertTrue(rim > 0, "no ejecta outside the crater")
			-- And far away it is exactly zero, which is what makes the cell index
			-- correct rather than approximate.
			h:assertEqual(edits:deltaAt(400, 400), 0)
		end)

		h:test("craters accumulate where they overlap", function()
			local edits = Edits.new()
			edits:crater(0, 0, 18, 8, 1)
			local single = edits:deltaAt(0, 0)
			edits:crater(4, 0, 18, 8, 2)
			h:assertTrue(edits:deltaAt(0, 0) < single, "a second crater did not deepen the first")
			h:assertEqual(edits:count(), 2)
		end)

		h:test("the version changes on every edit, so a renderer can compare one number", function()
			local edits = Edits.new()
			local before = edits:version()
			edits:crater(0, 0, 10, 5, 3)
			h:assertTrue(edits:version() > before)
		end)

		h:test("records round-trip so a client rebuilds the server's craters exactly", function()
			local edits = Edits.new()
			edits:crater(11, -6, 17, 9, 21)
			edits:crater(-30, 40, 12, 6, 22)

			local rebuilt = Edits.fromRecords(edits:records())
			h:assertEqual(rebuilt:count(), edits:count())
			for index = -60, 60, 3 do
				h:assertNear(rebuilt:deltaAt(index, index // 2), edits:deltaAt(index, index // 2), 1e-9)
			end
		end)

		h:test("chunksTouchedBy covers everywhere the crater reaches", function()
			-- A false negative here leaves a crater invisible until the chunk is
			-- rebuilt for some other reason, which reads as the ground healing.
			local edits = Edits.new()
			local crater = edits:crater(40, 40, 26, 12, 5)

			local touched: { [string]: boolean } = {}
			for _, pair in Edits.chunksTouchedBy(crater, CHUNK) do
				touched[`{pair[1]}:{pair[2]}`] = true
			end

			for z = -200, 200, 4 do
				for x = -200, 200, 4 do
					if math.abs(edits:deltaAt(x, z)) > 1e-9 then
						local key = `{math.floor(x / CHUNK)}:{math.floor(z / CHUNK)}`
						h:assertTrue(touched[key], `column {x},{z} changed in an unlisted chunk`)
					end
				end
			end
		end)

		h:test("a crater changes the chunk it lands in and scorches its floor", function()
			local edits = Edits.new()
			local field = HeightField.new({ seed = SEED }, edits)

			local before = Chunker.build(field, 0, 0, CHUNK)
			edits:crater(16, 16, 13, 9, 4)
			local after = Chunker.build(field, 0, 0, CHUNK)

			h:assertTrue(after.count ~= before.count, "the crater did not change the mesh")

			local scorched = 0
			for _, box in after.boxes do
				if box.surface == HeightField.SurfaceScorch then
					scorched += 1
				end
			end
			h:assertTrue(scorched > 0, "the crater floor is not scorched")

			for _, box in before.boxes do
				h:assertTrue(box.surface ~= HeightField.SurfaceScorch, "scorch appeared without an edit")
			end
		end)
	end)
end
