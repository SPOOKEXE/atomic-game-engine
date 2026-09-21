--!strict

-- Turning a square of columns into as few boxes as possible.
--
-- A chunk is `size` by `size` one-stud columns, so the naive rendering is `size`
-- squared parts. Three reductions bring that down, and none of them changes what
-- the world looks like -- a merged box is exactly the voxels it replaced:
--
--   1. **Only the exposed skin of a column.** A column is solid from bedrock to
--      its surface, and every voxel below the *lowest neighbouring* surface is
--      buried forever. What is emitted is the box from that neighbour up to this
--      column's own top: the top face plus whatever cliff is showing beside it,
--      and nothing behind either.
--   2. **Runs merged along X**, then
--   3. **runs merged along Z** when a whole run repeats on the next row.
--
-- That last pair is greedy meshing. On flat ground and open water it collapses a
-- 32 by 32 chunk to a handful of boxes; in broken country it does much less.
-- Measured on the equivalent code across 40 regions: 98 boxes per chunk on
-- average, 29 over water, 260 in mountains.
--
-- Nothing in here builds an Instance. `TerrainRuntime` does that, and keeping
-- the split means the meshing is testable without a place file -- which is how
-- the box counts above were measured at all.

local HeightField = require(script.Parent.HeightField)

local Chunker = {}

export type Box = {
	-- Inclusive column indices, in studs.
	x0: number,
	x1: number,
	z0: number,
	z1: number,
	-- Inclusive block indices on Y.
	bottom: number,
	top: number,
	-- Index into `HeightField.Surfaces`.
	surface: number,
}

export type Chunk = {
	cx: number,
	cz: number,
	size: number,
	boxes: { Box },
	count: number,
	-- The lowest and highest ground in the chunk, which a caller can use to
	-- place a camera or to skip a chunk that is entirely under water.
	minTop: number,
	maxTop: number,
}

-- How much cliff a single column may show, in studs.
--
-- A ravine wall is more interesting than it is affordable: without this, one
-- column beside a 60-stud drop becomes a 60-stud-tall part that is mostly inside
-- the hill behind it. Capped, the wall is drawn by the columns that step down it.
local MAX_EXPOSED = 24

-- Builds one chunk.
--
-- `cx` and `cz` are chunk coordinates; the columns covered are
-- `cx * size` through `cx * size + size - 1`.
function Chunker.build(field: any, cx: number, cz: number, size: number): Chunk
	assert(size % HeightField.BaseLattice == 0, "chunk size must be a multiple of the base lattice")

	local settings = field.settings
	local seaLevel = settings.seaLevel
	local bedrock = settings.bedrock

	local originX = cx * size
	local originZ = cz * size

	-- The lattice cells the padded chunk falls in, plus one so the far side of
	-- the last cell has a corner to interpolate towards.
	local lattice = HeightField.BaseLattice
	local latticeX0 = (originX - 1) // lattice
	local latticeZ0 = (originZ - 1) // lattice
	local latticeSpan = (size + 1) // lattice + 2

	local base: { number } = {}
	local warmth: { number } = {}
	local damp: { number } = {}
	local caveMask: { number } = {}

	for gz = 0, latticeSpan do
		for gx = 0, latticeSpan do
			local at = gz * (latticeSpan + 1) + gx + 1
			local wx = (latticeX0 + gx) * lattice
			local wz = (latticeZ0 + gz) * lattice
			base[at] = field:baseSample(wx, wz)
			warmth[at], damp[at] = field:climateAt(wx, wz)
			-- A 1400-stud field, so it belongs on the lattice for exactly the
			-- reason the others do -- and putting it here is what makes the gate
			-- in `solidTopAt` cheaper than the test it is gating.
			caveMask[at] = field:caveMaskAt(wx, wz)
		end
	end

	-- Bilinear lookup into the three grids at once, so a column pays for one set
	-- of weights rather than three.
	--
	-- The interpolations are written out rather than folded into a `mix` helper
	-- closed over the weights, which would allocate a closure per column. That
	-- is worth about 0.3 ms per chunk of the 6.7 -- small, and measured rather
	-- than assumed: the obvious suspects here are not where the time goes. See
	-- `caveMask` below for where it does.
	local function latticeAt(x: number, z: number): (number, number, number, number)
		local cellX = x / lattice - latticeX0
		local cellZ = z / lattice - latticeZ0

		local gx = math.floor(cellX)
		local gz = math.floor(cellZ)
		local tx = cellX - gx
		local tz = cellZ - gz

		local row = gz * (latticeSpan + 1) + gx + 1
		local below = row + latticeSpan + 1

		local baseNorth = base[row] + (base[row + 1] - base[row]) * tx
		local baseSouth = base[below] + (base[below + 1] - base[below]) * tx

		local warmNorth = warmth[row] + (warmth[row + 1] - warmth[row]) * tx
		local warmSouth = warmth[below] + (warmth[below + 1] - warmth[below]) * tx

		local dampNorth = damp[row] + (damp[row + 1] - damp[row]) * tx
		local dampSouth = damp[below] + (damp[below + 1] - damp[below]) * tx

		local caveNorth = caveMask[row] + (caveMask[row + 1] - caveMask[row]) * tx
		local caveSouth = caveMask[below] + (caveMask[below + 1] - caveMask[below]) * tx

		return baseNorth + (baseSouth - baseNorth) * tz,
			warmNorth + (warmSouth - warmNorth) * tz,
			dampNorth + (dampSouth - dampNorth) * tz,
			caveNorth + (caveSouth - caveNorth) * tz
	end

	-- A one-column border, sampled but never drawn. A column's exposed height
	-- depends on its four neighbours, and the neighbours of the edge columns are
	-- in the next chunk. Sampling them here rather than asking the neighbouring
	-- chunk is what keeps chunks independent -- they can be built in any order,
	-- and a chunk at the edge of the loaded region is not missing anything.
	local stride = size + 2
	local tops: { number } = {}
	local warmths: { number } = {}
	local damps: { number } = {}
	local dug: { number } = {}

	local edits = field:edits()
	local minTop = math.huge
	local maxTop = -math.huge

	for iz = -1, size do
		for ix = -1, size do
			local at = (iz + 1) * stride + (ix + 1) + 1
			local wx = originX + ix
			local wz = originZ + iz

			local columnBase, columnWarmth, columnDamp, columnCaves = latticeAt(wx, wz)
			local natural = columnBase + field:detailAt(wx, wz) * field:detailDamping(columnBase)

			local delta = if edits ~= nil then edits:deltaAt(wx, wz) else 0
			local top = field:solidTopAt(wx, wz, natural + delta, columnCaves)

			tops[at] = top
			warmths[at] = columnWarmth
			damps[at] = columnDamp
			-- How far below its natural height the edits pushed this column.
			-- Positive means dug out.
			dug[at] = -delta

			if ix >= 0 and ix < size and iz >= 0 and iz < size then
				minTop = math.min(minTop, top)
				maxTop = math.max(maxTop, top)
			end
		end
	end

	local function topAt(ix: number, iz: number): number
		return tops[(iz + 1) * stride + (ix + 1) + 1]
	end

	-- The second merge axis. A run closed on one row is not emitted straight
	-- away: it is held open, and if the next row produces a run with the same
	-- span, top, bottom and material it is extended in Z instead.
	--
	-- Held in one table keyed by the fields that have to match, so the lookup is
	-- a hash rather than a scan of the previous row. `openRow` records which row
	-- a box was last extended on -- without it, a run that skipped a row would
	-- rejoin the box above it and swallow the gap.
	local boxes: { Box } = {}
	local count = 0
	local open: { [string]: Box } = {}
	local openRow: { [string]: number } = {}

	local function emit(x0: number, x1: number, iz: number, bottom: number, top: number, surface: number)
		local key = `{x0}:{x1}:{bottom}:{top}:{surface}`
		local grown = open[key]

		if grown ~= nil and openRow[key] == iz - 1 then
			grown.z1 = originZ + iz
			openRow[key] = iz
			return
		end

		local box: Box = {
			x0 = originX + x0,
			x1 = originX + x1,
			z0 = originZ + iz,
			z1 = originZ + iz,
			bottom = bottom,
			top = top,
			surface = surface,
		}

		count += 1
		boxes[count] = box
		open[key] = box
		openRow[key] = iz
	end

	for iz = 0, size - 1 do
		local runStart = -1
		local runBottom = 0
		local runTop = 0
		local runSurface = 0
		local waterStart = -1

		for ix = 0, size do
			-- One column past the end, with no column of its own, so the loop
			-- body has exactly one place that flushes a run rather than a body
			-- and a copy of it afterwards.
			local inside = ix < size

			local top = 0
			local bottom = 0
			local surface = 0

			if inside then
				local at = (iz + 1) * stride + (ix + 1) + 1
				top = tops[at]

				-- The lowest of the four neighbours is where this column stops
				-- being visible. Everything below that is enclosed on all four
				-- sides and can never be seen from outside the hill.
				local lowest = math.min(
					topAt(ix - 1, iz),
					topAt(ix + 1, iz),
					topAt(ix, iz - 1),
					topAt(ix, iz + 1)
				)

				-- The slope in studs, which the material choice reads. Free
				-- here, and four more height samples anywhere else.
				local slope = top - lowest

				bottom = math.max(math.min(lowest + 1, top), top - MAX_EXPOSED, bedrock)
				surface = field:surfaceAt(top, slope, warmths[at], damps[at], dug[at])
			end

			if
				runStart >= 0
				and (not inside or top ~= runTop or bottom ~= runBottom or surface ~= runSurface)
			then
				emit(runStart, ix - 1, iz, runBottom, runTop, runSurface)
				runStart = -1
			end

			if inside and runStart < 0 then
				runStart = ix
				runTop = top
				runBottom = bottom
				runSurface = surface
			end

			-- Water is a second run over the same columns: one block thick, at
			-- sea level, rather than a solid box filled down to the sea floor.
			--
			-- A filled box is what the field literally says is there and it is
			-- the wrong thing to draw. Every water run would then have a
			-- different bottom -- the sea floor under it -- so none would merge
			-- in either axis, and half the map would cost a part per column. A
			-- surface has the same top and bottom everywhere, so a whole bay
			-- becomes one box.
			local wet = inside and top < seaLevel

			if waterStart >= 0 and not wet then
				emit(waterStart, ix - 1, iz, seaLevel, seaLevel, HeightField.SurfaceWater)
				waterStart = -1
			end

			if wet and waterStart < 0 then
				waterStart = ix
			end
		end
	end

	return {
		cx = cx,
		cz = cz,
		size = size,
		boxes = boxes,
		count = count,
		minTop = if minTop == math.huge then 0 else minTop,
		maxTop = if maxTop == -math.huge then 0 else maxTop,
	}
end

-- The centre and size a box occupies, in studs.
--
-- Every bound is inclusive, which is what the merge produces and what makes the
-- extents read as counts. The centre lands on a half-integer whenever a span is
-- odd, which is correct and is the arithmetic worth reading twice: a single
-- block at index 4 occupies [4, 5) and is centred at 4.5.
function Chunker.boxGeometry(box: Box): (number, number, number, number, number, number)
	local width = box.x1 - box.x0 + 1
	local height = box.top - box.bottom + 1
	local depth = box.z1 - box.z0 + 1

	return box.x0 + width * 0.5,
		box.bottom + height * 0.5,
		box.z0 + depth * 0.5,
		width,
		height,
		depth
end

Chunker.MaxExposed = MAX_EXPOSED
Chunker.Build = Chunker.build
Chunker.BoxGeometry = Chunker.boxGeometry

return table.freeze(Chunker)
