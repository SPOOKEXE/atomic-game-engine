--!strict

-- Noise sampled onto a grid, in 2D and in 3D.
--
-- `Noise` answers one question at a time: what is the field at this point. That
-- is the right shape for a height field, which asks about one column and throws
-- the answer away. It is the wrong shape for three other jobs, and this module
-- is those three.
--
-- **Caching a field that is too expensive to ask twice.** A five-octave ridged
-- fBm behind a domain warp is thirty hashes a sample. Anything that reads the
-- same square kilometre repeatedly -- a minimap, a biome pass that wants its
-- neighbours, a spawner scanning for flat ground -- should sample it once onto a
-- grid and interpolate. `sample2` is that read, and it is a lerp rather than a
-- field evaluation.
--
-- **Looking at it.** A noise field is one of the few things in a game where the
-- bug and the picture are the same object: a tiling artefact, a field stuck near
-- zero, an fBm that never leaves the middle of its range are all obvious at a
-- glance and all invisible in a number. Atlas draws its maps into an
-- `EditableImage`; there is no such thing here and `TerrainCore` would not be
-- allowed to touch one anyway, so `toText2` renders to a string and `histogram2`
-- to counts. Both work in a headless test, which is the point -- a picture you
-- can only get by launching the game is a picture nobody looks at.
--
-- **Knowing the range before you use it.** `build2` records the minimum and
-- maximum it actually saw, so `normalized2` can map a field onto [0, 1] using
-- its measured extremes rather than its nominal ones. This is the same trap
-- `FBM_GAIN` exists for, one level up: a field that theoretically spans [-1, 1]
-- and practically spans [-0.5, 0.5] produces a washed-out map, and the fix is to
-- measure rather than to assume.
--
--     local map = NoiseMap.build2({
--         width = 256,
--         depth = 256,
--         step = 4,
--         sample = NoiseMap.field2({ kind = "simplexFbm", seed = 7, scale = 400, octaves = 5 }),
--     })
--
--     print(NoiseMap.toText2(NoiseMap.normalized2(map)))
--
-- Every function here is pure: `build2` calls the sampler it was handed and
-- nothing else, so a map is reproducible from its spec and two machines building
-- the same map get the same numbers.

local Noise = require(script.Parent.Noise)

local NoiseMap = {}

-- A 2D map. `values` is row-major and 1-based: the cell at column `c`, row `r`
-- lives at `(r - 1) * width + c`.
--
-- Flat rather than a table of row tables. A 256x256 map is one allocation
-- instead of 257, and the row tables buy nothing -- nobody passes a single row
-- anywhere, and `sample2` reads two rows at once regardless.
export type Map2 = {
	width: number,
	depth: number,
	originX: number,
	originZ: number,
	step: number,
	min: number,
	max: number,
	values: { number },
}

-- A 3D map, layer-major over rows over columns: the cell at column `c`, row `r`,
-- layer `l` lives at `((l - 1) * depth + (r - 1)) * width + c`.
--
-- Layers are the Y axis, so `slice3` walks a contiguous run and a map read
-- bottom-up -- which is how a cave mesher reads a density field -- touches
-- memory in order.
export type Map3 = {
	width: number,
	height: number,
	depth: number,
	originX: number,
	originY: number,
	originZ: number,
	step: number,
	min: number,
	max: number,
	values: { number },
}

export type Spec2 = {
	width: number,
	depth: number,
	sample: (number, number) -> number,
	originX: number?,
	originZ: number?,
	step: number?,
}

export type Spec3 = {
	width: number,
	height: number,
	depth: number,
	sample: (number, number, number) -> number,
	originX: number?,
	originY: number?,
	originZ: number?,
	step: number?,
}

-- What `field2` and `field3` build a sampler from.
--
-- `scale` is a world distance -- the size of the largest feature -- rather than
-- a frequency, because every caller has the first number and none of them has
-- the second. A scale of 400 means the coarsest octave turns over roughly every
-- 400 studs; the reciprocal is what the noise functions actually want and this
-- is the only place it gets taken.
export type FieldSpec = {
	kind: string,
	seed: number,
	scale: number?,
	octaves: number?,
	lacunarity: number?,
}

local DEFAULT_SCALE = 100
local DEFAULT_OCTAVES = 4
local DEFAULT_LACUNARITY = 2

-- The glyphs `toText2` ramps through, darkest last. Ten of them, because a
-- terminal cell is not a pixel and pretending otherwise reads as noise about the
-- noise.
local RAMP = { " ", ".", ":", "-", "=", "+", "*", "#", "%", "@" }

local function positiveInteger(value: number, name: string): number
	assert(value == value // 1 and value >= 1, `NoiseMap: {name} must be a positive integer`)
	return value
end

-- ## Building

-- Samples a field onto a 2D grid, recording the range it actually covered.
--
-- The sampler is called exactly `width * depth` times, in row-major order, at
-- `origin + index * step`. That order is not decoration: a sampler that carries
-- state across calls -- a cache of the last cell, say -- gets its calls in the
-- order it would want them, and one that does not is unaffected.
function NoiseMap.build2(spec: Spec2): Map2
	local width = positiveInteger(spec.width, "width")
	local depth = positiveInteger(spec.depth, "depth")
	local originX = spec.originX or 0
	local originZ = spec.originZ or 0
	local step = spec.step or 1
	local sample = spec.sample

	local values = table.create(width * depth)
	local min = math.huge
	local max = -math.huge

	for row = 1, depth do
		local z = originZ + (row - 1) * step
		local base = (row - 1) * width
		for column = 1, width do
			local value = sample(originX + (column - 1) * step, z)
			values[base + column] = value
			if value < min then
				min = value
			end
			if value > max then
				max = value
			end
		end
	end

	return {
		width = width,
		depth = depth,
		originX = originX,
		originZ = originZ,
		step = step,
		min = min,
		max = max,
		values = values,
	}
end

-- The same, one axis deeper. A 128-cube is two million samples, which is seconds
-- rather than milliseconds -- 3D maps are for a chunk's worth of density, not
-- for a world.
function NoiseMap.build3(spec: Spec3): Map3
	local width = positiveInteger(spec.width, "width")
	local height = positiveInteger(spec.height, "height")
	local depth = positiveInteger(spec.depth, "depth")
	local originX = spec.originX or 0
	local originY = spec.originY or 0
	local originZ = spec.originZ or 0
	local step = spec.step or 1
	local sample = spec.sample

	local values = table.create(width * height * depth)
	local min = math.huge
	local max = -math.huge

	for layer = 1, height do
		local y = originY + (layer - 1) * step
		for row = 1, depth do
			local z = originZ + (row - 1) * step
			local base = ((layer - 1) * depth + (row - 1)) * width
			for column = 1, width do
				local value = sample(originX + (column - 1) * step, y, z)
				values[base + column] = value
				if value < min then
					min = value
				end
				if value > max then
					max = value
				end
			end
		end
	end

	return {
		width = width,
		height = height,
		depth = depth,
		originX = originX,
		originY = originY,
		originZ = originZ,
		step = step,
		min = min,
		max = max,
		values = values,
	}
end

-- ## Reading

-- The cell at a 1-based column and row. Nil outside the map rather than an
-- error, so a neighbour scan at the edge reads as absent instead of throwing.
function NoiseMap.at2(map: Map2, column: number, row: number): number?
	if column < 1 or column > map.width or row < 1 or row > map.depth then
		return nil
	end
	return map.values[(row - 1) * map.width + column]
end

function NoiseMap.at3(map: Map3, column: number, row: number, layer: number): number?
	if
		column < 1
		or column > map.width
		or row < 1
		or row > map.depth
		or layer < 1
		or layer > map.height
	then
		return nil
	end
	return map.values[((layer - 1) * map.depth + (row - 1)) * map.width + column]
end

-- The map read as a continuous field, bilinearly, in the world coordinates it
-- was built from.
--
-- **Clamped at the edges, not wrapped and not extrapolated.** A map is a window
-- onto a field and the field outside it is not zero -- it is unknown. Clamping
-- makes the border extend outward, which is wrong in a way that looks like a
-- flat plain; extrapolating would be wrong in a way that looks like a cliff off
-- the end of the world, and wrapping would be wrong in a way that looks correct
-- until two windows are put side by side.
--
-- Bilinear rather than the quintic fade the noise itself uses. The grid is
-- already a low-pass of a smooth field, so the crease bilinear leaves on cell
-- lines is well under the error the sampling introduced -- and this is the read
-- that has to be cheaper than re-evaluating the field, or the map was pointless.
function NoiseMap.sample2(map: Map2, x: number, z: number): number
	local width = map.width
	local depth = map.depth

	local cx = math.clamp((x - map.originX) / map.step + 1, 1, width)
	local cz = math.clamp((z - map.originZ) / map.step + 1, 1, depth)

	local column = math.floor(cx)
	local row = math.floor(cz)
	local nextColumn = math.min(column + 1, width)
	local nextRow = math.min(row + 1, depth)

	local tx = cx - column
	local tz = cz - row

	local values = map.values
	local base = (row - 1) * width
	local nextBase = (nextRow - 1) * width

	local north = values[base + column] + (values[base + nextColumn] - values[base + column]) * tx
	local south = values[nextBase + column] + (values[nextBase + nextColumn] - values[nextBase + column]) * tx
	return north + (south - north) * tz
end

-- Trilinear, on the same terms.
function NoiseMap.sample3(map: Map3, x: number, y: number, z: number): number
	local width = map.width
	local height = map.height
	local depth = map.depth

	local cx = math.clamp((x - map.originX) / map.step + 1, 1, width)
	local cy = math.clamp((y - map.originY) / map.step + 1, 1, height)
	local cz = math.clamp((z - map.originZ) / map.step + 1, 1, depth)

	local column = math.floor(cx)
	local layer = math.floor(cy)
	local row = math.floor(cz)
	local nextColumn = math.min(column + 1, width)
	local nextLayer = math.min(layer + 1, height)
	local nextRow = math.min(row + 1, depth)

	local tx = cx - column
	local ty = cy - layer
	local tz = cz - row

	local values = map.values
	local stride = depth * width

	-- The bilinear read of one layer, written out twice rather than lifted into
	-- a local function called for each. A closure that captures eleven locals is
	-- allocated on every call to the function containing it, and this is the
	-- read that exists to be cheaper than evaluating the field.
	local lowBase = ((layer - 1) * depth + (row - 1)) * width
	local lowNextBase = lowBase + (nextRow - row) * width
	local lowNorth = values[lowBase + column] + (values[lowBase + nextColumn] - values[lowBase + column]) * tx
	local lowSouth = values[lowNextBase + column]
		+ (values[lowNextBase + nextColumn] - values[lowNextBase + column]) * tx
	local low = lowNorth + (lowSouth - lowNorth) * tz

	local highBase = lowBase + (nextLayer - layer) * stride
	local highNextBase = highBase + (nextRow - row) * width
	local highNorth = values[highBase + column] + (values[highBase + nextColumn] - values[highBase + column]) * tx
	local highSouth = values[highNextBase + column]
		+ (values[highNextBase + nextColumn] - values[highNextBase + column]) * tx
	local high = highNorth + (highSouth - highNorth) * tz

	return low + (high - low) * ty
end

-- ## Reshaping

-- The map with its values on [0, 1], stretched from the extremes it actually
-- reached rather than the ones its field nominally spans.
--
-- A constant map maps to all zeroes rather than dividing by nothing. That is a
-- real case -- a Worley `f2 - f1` on a grid coarser than its cells, or any field
-- sampled at exactly its period -- and it is a bug in the caller's spec, which a
-- flat map shows and a NaN map hides.
function NoiseMap.normalized2(map: Map2): Map2
	local span = map.max - map.min
	local scale = if span > 0 then 1 / span else 0
	local low = map.min

	local source = map.values
	local values = table.create(#source)
	for index = 1, #source do
		values[index] = (source[index] - low) * scale
	end

	return {
		width = map.width,
		depth = map.depth,
		originX = map.originX,
		originZ = map.originZ,
		step = map.step,
		min = 0,
		max = if span > 0 then 1 else 0,
		values = values,
	}
end

-- Every cell through a function, with the range remeasured afterwards.
--
-- Remeasured rather than transformed, because the function is arbitrary: it may
-- be non-monotonic, and then the old extremes are not the new ones and a map
-- carrying stale bounds silently breaks `normalized2` and `toText2` downstream.
function NoiseMap.remapped2(map: Map2, transform: (number) -> number): Map2
	local source = map.values
	local values = table.create(#source)
	local min = math.huge
	local max = -math.huge

	for index = 1, #source do
		local value = transform(source[index])
		values[index] = value
		if value < min then
			min = value
		end
		if value > max then
			max = value
		end
	end

	return {
		width = map.width,
		depth = map.depth,
		originX = map.originX,
		originZ = map.originZ,
		step = map.step,
		min = min,
		max = max,
		values = values,
	}
end

-- One horizontal layer of a 3D map, as a 2D map. The layer is 1-based from the
-- bottom, and its Y is dropped -- a `Map2` is a plan view and has nowhere to
-- keep it.
function NoiseMap.slice3(map: Map3, layer: number): Map2
	assert(layer >= 1 and layer <= map.height, "NoiseMap.slice3: layer is outside the map")

	local width = map.width
	local depth = map.depth
	local source = map.values
	local offset = (layer - 1) * depth * width

	local values = table.create(width * depth)
	local min = math.huge
	local max = -math.huge

	for index = 1, width * depth do
		local value = source[offset + index]
		values[index] = value
		if value < min then
			min = value
		end
		if value > max then
			max = value
		end
	end

	return {
		width = width,
		depth = depth,
		originX = map.originX,
		originZ = map.originZ,
		step = map.step,
		min = min,
		max = max,
		values = values,
	}
end

-- ## Looking

-- How many cells fall in each of `buckets` equal slices of [min, max].
--
-- The cheapest test that catches the failure a range check misses: a field can
-- span its whole range and still spend 95% of its samples in the middle two
-- percent of it, which on screen is a flat plain with a couple of spikes. The
-- histogram says so in one line and nothing else does.
function NoiseMap.histogram2(map: Map2, buckets: number): { number }
	positiveInteger(buckets, "buckets")

	local counts = table.create(buckets, 0)
	for index = 1, buckets do
		counts[index] = 0
	end

	local span = map.max - map.min
	if span <= 0 then
		counts[1] = #map.values
		return counts
	end

	local scale = buckets / span
	local low = map.min
	for _, value in map.values do
		local bucket = math.min(math.floor((value - low) * scale) + 1, buckets)
		counts[bucket] += 1
	end

	return counts
end

-- The map as text, one glyph per cell, dark for low.
--
-- Values are read against `min` and `max`, so a map that has not been normalised
-- still renders across the full ramp. Rows are joined with newlines and the
-- first row is the one at `originZ`, which means the picture reads north-up the
-- way a plan view does rather than upside down the way an image buffer does.
function NoiseMap.toText2(map: Map2): string
	local span = map.max - map.min
	local scale = if span > 0 then #RAMP / span else 0
	local low = map.min
	local values = map.values
	local width = map.width

	local rows = table.create(map.depth)
	local cells = table.create(width)

	for row = 1, map.depth do
		local base = (row - 1) * width
		for column = 1, width do
			local level = math.clamp(math.floor((values[base + column] - low) * scale) + 1, 1, #RAMP)
			cells[column] = RAMP[level]
		end
		rows[row] = table.concat(cells)
	end

	return table.concat(rows, "\n")
end

-- ## Fields

-- A sampler for `build2`, from a description rather than a closure the caller
-- writes by hand.
--
-- The point is not brevity. Every one of these fields needs its coordinates
-- divided by a scale and most of them need octave counts threaded through, and
-- that arithmetic written out at each call site is where the seed ends up
-- shared between two fields that were meant to differ, or where one of them is
-- silently sampled at a hundred times the intended frequency.
--
-- `worley` is F1, the raw distance to the nearest feature point; `worleyEdge` is
-- F2 - F1, which is the cell walls. Both are in [0, ~1.4] rather than [-1, 1] --
-- they are distances, and normalising them here would cost the contrast that
-- makes them worth having.
function NoiseMap.field2(spec: FieldSpec): (number, number) -> number
	local seed = spec.seed
	local scale = spec.scale or DEFAULT_SCALE
	local octaves = spec.octaves or DEFAULT_OCTAVES
	local lacunarity = spec.lacunarity or DEFAULT_LACUNARITY
	local inverse = 1 / scale
	local kind = spec.kind

	if kind == "perlin" then
		return function(x: number, z: number): number
			return Noise.noise2(x * inverse, z * inverse, seed)
		end
	elseif kind == "simplex" then
		return function(x: number, z: number): number
			return Noise.simplex2(x * inverse, z * inverse, seed)
		end
	elseif kind == "value" then
		return function(x: number, z: number): number
			return Noise.value2(math.floor(x * inverse), math.floor(z * inverse), seed)
		end
	elseif kind == "fbm" then
		return function(x: number, z: number): number
			return Noise.fbm2(x * inverse, z * inverse, seed, octaves, lacunarity)
		end
	elseif kind == "simplexFbm" then
		return function(x: number, z: number): number
			return Noise.simplexFbm2(x * inverse, z * inverse, seed, octaves, lacunarity)
		end
	elseif kind == "ridged" then
		return function(x: number, z: number): number
			return Noise.ridged2(x * inverse, z * inverse, seed, octaves)
		end
	elseif kind == "worley" then
		return function(x: number, z: number): number
			local first = Noise.worley2(x * inverse, z * inverse, seed)
			return first
		end
	elseif kind == "worleyEdge" then
		return function(x: number, z: number): number
			local first, second = Noise.worley2(x * inverse, z * inverse, seed)
			return second - first
		end
	end

	error(`NoiseMap.field2: unknown kind "{kind}"`)
end

function NoiseMap.field3(spec: FieldSpec): (number, number, number) -> number
	local seed = spec.seed
	local scale = spec.scale or DEFAULT_SCALE
	local octaves = spec.octaves or DEFAULT_OCTAVES
	local lacunarity = spec.lacunarity or DEFAULT_LACUNARITY
	local inverse = 1 / scale
	local kind = spec.kind

	if kind == "value" then
		return function(x: number, y: number, z: number): number
			return Noise.noise3(x * inverse, y * inverse, z * inverse, seed)
		end
	elseif kind == "simplex" then
		return function(x: number, y: number, z: number): number
			return Noise.simplex3(x * inverse, y * inverse, z * inverse, seed)
		end
	elseif kind == "fbm" then
		return function(x: number, y: number, z: number): number
			return Noise.fbm3(x * inverse, y * inverse, z * inverse, seed, octaves, lacunarity)
		end
	elseif kind == "simplexFbm" then
		return function(x: number, y: number, z: number): number
			return Noise.simplexFbm3(x * inverse, y * inverse, z * inverse, seed, octaves, lacunarity)
		end
	elseif kind == "worley" then
		return function(x: number, y: number, z: number): number
			local first = Noise.worley3(x * inverse, y * inverse, z * inverse, seed)
			return first
		end
	elseif kind == "worleyEdge" then
		return function(x: number, y: number, z: number): number
			local first, second = Noise.worley3(x * inverse, y * inverse, z * inverse, seed)
			return second - first
		end
	end

	error(`NoiseMap.field3: unknown kind "{kind}"`)
end

NoiseMap.Build2 = NoiseMap.build2
NoiseMap.Build3 = NoiseMap.build3
NoiseMap.At2 = NoiseMap.at2
NoiseMap.At3 = NoiseMap.at3
NoiseMap.Sample2 = NoiseMap.sample2
NoiseMap.Sample3 = NoiseMap.sample3
NoiseMap.Normalized2 = NoiseMap.normalized2
NoiseMap.Remapped2 = NoiseMap.remapped2
NoiseMap.Slice3 = NoiseMap.slice3
NoiseMap.Histogram2 = NoiseMap.histogram2
NoiseMap.ToText2 = NoiseMap.toText2
NoiseMap.Field2 = NoiseMap.field2
NoiseMap.Field3 = NoiseMap.field3

return table.freeze(NoiseMap)
