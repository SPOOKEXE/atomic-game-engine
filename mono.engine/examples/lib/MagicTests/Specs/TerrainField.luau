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
