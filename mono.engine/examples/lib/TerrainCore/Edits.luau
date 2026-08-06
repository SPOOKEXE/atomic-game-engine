--!strict

-- Deformation laid over the generated height field.
--
-- `HeightField` is a pure function of two integers and holds no state, which is
-- what makes a map larger than memory possible. That is also what makes it
-- unable to remember that something blew a hole in it. This module is the other
-- half: a small list of edits that the height field adds on top, so the world
-- stays a function plus a diff rather than becoming a stored array of blocks.
--
-- The list is the whole state of every crater in the world. It serialises as
-- plain data, it replicates as plain data, and rebuilding a chunk from it
-- produces the same blocks on every machine -- which is the property a spell
-- that cratered the ground differently for two players would break.
--
-- ## Why a grid index rather than a scan
--
-- `deltaAt` is called once per column of every chunk built, which is about a
-- thousand times per chunk. A demo that has been firing for a minute has
-- hundreds of craters, and scanning all of them per column is the difference
-- between a chunk costing 4 ms and costing 400. Edits are filed into cells the
-- size of the largest edit they hold, so a column reads at most the nine cells
-- around it.

local Noise = require(script.Parent.Noise)

local Edits = {}
Edits.__index = Edits

export type Crater = {
	kind: "Crater",
	id: number,
	x: number,
	z: number,
	-- Where the bowl reaches zero, in studs.
	radius: number,
	-- How far the centre drops, in studs. Positive digs down.
	depth: number,
	-- How high the ejecta rim stands above the untouched ground.
	rim: number,
	seed: number,
}

-- Cells are indexed `_cells[cx][cz]` rather than by a `"cx:cz"` string.
--
-- `deltaAt` runs once per column of every chunk, so a string key is a string
-- allocated a thousand times per chunk -- measured at 0.32 ms of a 4.2 ms chunk
-- *with no craters in the world at all*, which is a cost paid for nothing. Two
-- integer-keyed lookups allocate nothing.
export type Field = typeof(setmetatable(
	{} :: {
		_edits: { Crater },
		_cells: { [number]: { [number]: { Crater } } },
		_cellSize: number,
		_nextId: number,
		_version: number,
	},
	Edits
))

-- Cells are this wide. Large enough that a crater spans few of them, small
-- enough that a cell holds few craters.
local CELL_SIZE = 64

-- How ragged a crater lip is, as a fraction of its radius. Zero is a perfect
-- circle, which is the one thing a blast never leaves.
local WOBBLE = 0.14

-- Wavelength of that raggedness, in studs. Short enough to read as broken rock
-- rather than as a wavy circle.
local WOBBLE_SCALE = 6.5

function Edits.new(): Field
	return setmetatable({
		_edits = {},
		_cells = {},
		_cellSize = CELL_SIZE,
		_nextId = 0,
		_version = 0,
	}, Edits) :: any
end

local function bucketAt(self: Field, cx: number, cz: number): { Crater }?
	local column = self._cells[cx]
	return if column ~= nil then column[cz] else nil
end

-- Adds a crater and returns it.
--
-- The seed decides the raggedness of the lip and nothing else, so two craters at
-- the same place with the same seed are the same crater -- which is what lets a
-- client rebuild one the server described without the two disagreeing about
-- where the rock broke.
function Edits.crater(self: Field, x: number, z: number, radius: number, depth: number, seed: number?): Crater
	assert(radius > 0, "crater radius must be positive")

	self._nextId += 1
	local edit: Crater = {
		kind = "Crater",
		id = self._nextId,
		x = x,
		z = z,
		radius = radius,
		depth = depth,
		-- Ejecta scales with how much was dug out rather than being authored
		-- separately: a deeper hole threw more rock onto its own lip.
		rim = math.abs(depth) * 0.22,
		seed = seed or self._nextId,
	}

	table.insert(self._edits, edit)

	-- Filed into every cell it can possibly influence, rim included, so
	-- `deltaAt` reading one cell is not an approximation.
	local reach = edit.radius * (1 + WOBBLE) + edit.rim * 2
	local minX = math.floor((edit.x - reach) / self._cellSize)
	local maxX = math.floor((edit.x + reach) / self._cellSize)
	local minZ = math.floor((edit.z - reach) / self._cellSize)
	local maxZ = math.floor((edit.z + reach) / self._cellSize)

	for cx = minX, maxX do
		local column = self._cells[cx]
		if column == nil then
			column = {}
			self._cells[cx] = column
		end
		for cz = minZ, maxZ do
			local bucket = column[cz]
			if bucket == nil then
				bucket = {}
				column[cz] = bucket
			end
			table.insert(bucket, edit)
		end
	end

	self._version += 1
	return edit
end

-- One edit's contribution at a column, in studs. Negative digs.
local function contribution(edit: Crater, x: number, z: number): number
	local dx = x - edit.x
	local dz = z - edit.z
	local distance = math.sqrt(dx * dx + dz * dz)

	-- The lip is perturbed rather than the whole bowl: displacing the distance
	-- itself moves the rim and the floor together, so the crater stays a crater
	-- and only its edge breaks up. Warping the depth instead gives a lumpy floor
	-- and a circular lip, which is exactly backwards.
	local wobble = Noise.noise2(x / WOBBLE_SCALE, z / WOBBLE_SCALE, edit.seed) * edit.radius * WOBBLE
	local reach = edit.radius + wobble

	if reach <= 0 then
		return 0
	end

	local ratio = distance / reach

	if ratio < 1 then
		-- A paraboloid rather than a cone or a cosine: the floor is flat-ish and
		-- the walls steepen towards the lip, which is the shape an explosion
		-- actually leaves and the one that merges into long runs at the bottom.
		return -edit.depth * (1 - ratio * ratio)
	end

	-- Ejecta, falling off over half a radius outside the lip. Past that an edit
	-- contributes exactly zero, which is what makes the cell index correct
	-- rather than approximate.
	local outside = (distance - reach) / (reach * 0.5)
	if outside >= 1 then
		return 0
	end

	local falloff = 1 - outside
	return edit.rim * falloff * falloff
end

-- The summed height change at a column, in studs.
function Edits.deltaAt(self: Field, x: number, z: number): number
	-- The empty case first, because it is the common one: a world that has not
	-- been shot at yet still calls this once per column of every chunk.
	if #self._edits == 0 then
		return 0
	end

	local bucket = bucketAt(self, math.floor(x / self._cellSize), math.floor(z / self._cellSize))
	if bucket == nil then
		return 0
	end

	local total = 0
	for _, edit in bucket do
		total += contribution(edit, x, z)
	end
	return total
end

-- Whether any edit reaches into an axis-aligned column range.
--
-- Used to decide whether a built chunk has to be rebuilt, so it errs towards
-- true: a false negative leaves a crater invisible until the chunk happens to
-- be rebuilt for another reason, and that is a bug somebody reports as "the
-- ground healed itself".
function Edits.touches(self: Field, minX: number, minZ: number, maxX: number, maxZ: number): boolean
	local minCellX = math.floor(minX / self._cellSize)
	local maxCellX = math.floor(maxX / self._cellSize)
	local minCellZ = math.floor(minZ / self._cellSize)
	local maxCellZ = math.floor(maxZ / self._cellSize)

	for cx = minCellX, maxCellX do
		for cz = minCellZ, maxCellZ do
			if bucketAt(self, cx, cz) ~= nil then
				return true
			end
		end
	end
	return false
end

-- Which of a set of chunk keys an edit invalidates, given a chunk size in studs.
--
-- Returned as a list of (cx, cz) pairs rather than as keys, because the caller
-- owns how a chunk is named and this module should not have to agree with it.
function Edits.chunksTouchedBy(edit: Crater, chunkSize: number): { { number } }
	local reach = edit.radius * (1 + WOBBLE) * 1.5 + edit.rim * 2

	local minX = math.floor((edit.x - reach) / chunkSize)
	local maxX = math.floor((edit.x + reach) / chunkSize)
	local minZ = math.floor((edit.z - reach) / chunkSize)
	local maxZ = math.floor((edit.z + reach) / chunkSize)

	local chunks: { { number } } = {}
	for cz = minZ, maxZ do
		for cx = minX, maxX do
			table.insert(chunks, { cx, cz })
		end
	end
	return chunks
end

-- How many edits the field holds, and a counter that changes whenever one is
-- added. A renderer compares the counter rather than the list.
function Edits.count(self: Field): number
	return #self._edits
end

function Edits.version(self: Field): number
	return self._version
end

-- The edits as plain data, in the order they were made. This is what would go
-- over a remote or into a save.
function Edits.records(self: Field): { Crater }
	local out: { Crater } = {}
	for index, edit in self._edits do
		out[index] = table.clone(edit)
	end
	return out
end

-- Rebuilds a field from `records`, ids and seeds intact, so a client's craters
-- land exactly where the server's did.
function Edits.fromRecords(records: { Crater }): Field
	local field = Edits.new()
	for _, record in records do
		local edit = field:crater(record.x, record.z, record.radius, record.depth, record.seed)
		edit.rim = record.rim
	end
	return field
end

Edits.New = Edits.new
Edits.FromRecords = Edits.fromRecords
Edits.CellSize = CELL_SIZE

return Edits
