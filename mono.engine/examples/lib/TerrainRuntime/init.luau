--!strict

-- Turning `TerrainCore` boxes into Parts.
--
-- `TerrainCore` computes and never touches an instance, which is what let its
-- whole test suite run with no world at all. This is the other side of that
-- line, and it is the only file in the pair that names `Part` or
-- `Color3`.
--
-- ## What differs from the Roblox original
--
-- Two things, and neither is in the core:
--
--   * **Materials.** `TerrainCore` names a material as a string because it has
--     to run outside any engine. Nothing here reads that name any more: v0.10
--     removed `Enum.Material`, and the replacement names a published asset,
--     which a demo cannot assume is on the machine running it. The colour is
--     the core's either way, and colour is what actually reads at terrain
--     scale.
--   * **No `Folder`.** A bare `Instance` is the container, which is all a
--     `Folder` ever was.

-- **A sibling, not a service.** This module and `TerrainCore` are mounted
-- together as children of whichever script uses them, so the way to reach one
-- from the other is the relative path every module inside `TerrainCore` already
-- uses. Naming `ReplicatedStorage` made this the one file in the pair that only
-- worked while somebody was mirroring the libraries into every world.
local Siblings = (script :: any).Parent

local Terrain = require(Siblings.TerrainCore)

local Chunker = Terrain.Chunker
local HeightField = Terrain.HeightField

local TerrainRuntime = {}
TerrainRuntime.__index = TerrainRuntime

-- How many idle parts the pool may hold before the rest are destroyed.
--
-- A high-water-mark pool is not free: how many boxes a chunk needs depends
-- entirely on the terrain in it -- measured 29 over open water and 260 in broken
-- mountains -- so a camera that crosses a range and then flies out over an ocean
-- would otherwise leave the pool holding the difference forever.
local POOL_LIMIT = 2048

-- **No material is set, and `Enum.Material` no longer exists.** v0.10 removed
-- it: seventeen names nothing sampled, replaced by a `Material` instance that
-- names a published asset. A demo that shipped one would only draw right on a
-- machine whose content store happened to hold that material, so the colour
-- carries the look here exactly as it always actually did.

local WATER = HeightField.SurfaceWater
local WATER_TRANSPARENCY = 0.35

export type Builder = typeof(setmetatable(
	{} :: {
		field: any,
		size: number,
		container: Instance,
		_chunks: { [string]: { parts: { BasePart }, count: number } },
		_pool: { BasePart },
		_poolCount: number,
		_created: number,
		_destroyed: number,
		_loaded: number,
		-- Chunks an edit has invalidated and `pump` has not rebuilt yet, oldest
		-- first, with a set beside it so one chunk is queued once however many
		-- craters land on it.
		_queue: { { number } },
		_queued: { [string]: boolean },
	},
	TerrainRuntime
))

local COLOURS: { Color3 } = {}
for index, surface in HeightField.Surfaces do
	COLOURS[index] = Color3.new(surface.color.r, surface.color.g, surface.color.b)
end

-- Packs an integer column coordinate pair into one number.
--
-- A number key rather than `"{x}:{z}"`, because this is looked up about three
-- thousand times per chunk rebuild and a string key would allocate every one of
-- them. Exact for any coordinate a place can reach: a double holds integers to
-- 2^53 and this uses 2^24 of that for `z`.
local function columnKey(x: number, z: number): number
	return x * 16777216 + z
end

-- A height field that remembers the half of itself an edit cannot change.
--
-- **Every sampler wrapped here is a pure function of position and seed.**
-- `baseSample`, `climateAt`, `caveMaskAt` and `detailAt` read noise and nothing
-- else - `HeightField.heightAt` is `naturalAt` *plus* `edits:deltaAt`, and the
-- edit half is applied by `Chunker` on top of what these return. So a crater
-- cannot make any of these answers stale, and this cache never needs
-- invalidating. That is the whole reason it is worth having: a chunk rebuilt for
-- the tenth time re-samples nothing.
--
-- It is worth having because a rebuild is almost all noise. One 32-stud chunk
-- samples 49 lattice points at about twenty-five octaves each, then 1156 columns
-- at two more - some three thousand noise evaluations to redraw a hole a few
-- studs across.
--
-- **Bounded by the loaded region and not by time.** An entry exists per lattice
-- point and per column of every chunk that has ever been built, which for an
-- arena is a fixed number - about sixty thousand entries for the demo's 49
-- chunks. A game that streamed terrain without bound would want an eviction
-- policy; an arena does not, and inventing one here would be a cache of a cache.
--
-- Wrapped rather than folded into `HeightField`, because `TerrainCore` is
-- byte-identical to the project it was ported from and stays that way. Every
-- method this does not name resolves straight through to the real field.
local function remembering(field: any): any
	local base: { [number]: number } = {}
	local warmth: { [number]: number } = {}
	local damp: { [number]: number } = {}
	local caves: { [number]: number } = {}
	local detail: { [number]: number } = {}

	local proxy = setmetatable({}, { __index = field })

	function proxy.baseSample(_self: any, x: number, z: number): number
		local key = columnKey(x, z)
		local held = base[key]
		if held == nil then
			held = field:baseSample(x, z)
			base[key] = held
		end
		return held
	end

	-- Two values, so two tables and one presence test. `warmth` is the one
	-- tested because both are written together and neither is ever nil.
	function proxy.climateAt(_self: any, x: number, z: number): (number, number)
		local key = columnKey(x, z)
		local held = warmth[key]
		if held == nil then
			local warm, wet = field:climateAt(x, z)
			warmth[key] = warm
			damp[key] = wet
			return warm, wet
		end
		return held, damp[key]
	end

	function proxy.caveMaskAt(_self: any, x: number, z: number): number
		local key = columnKey(x, z)
		local held = caves[key]
		if held == nil then
			held = field:caveMaskAt(x, z)
			caves[key] = held
		end
		return held
	end

	-- The one called per column rather than per lattice point, so most of the
	-- entries and most of the saving are here.
	function proxy.detailAt(_self: any, x: number, z: number): number
		local key = columnKey(x, z)
		local held = detail[key]
		if held == nil then
			held = field:detailAt(x, z)
			detail[key] = held
		end
		return held
	end

	-- **The dug half, cached the same way and invalidated by hand.**
	--
	-- Unlike everything above, this one *does* go stale: a crater is exactly the
	-- thing `deltaAt` reports. It is cached anyway because it is the single most
	-- expensive thing in a rebuild once an arena has been shot at for a while.
	-- `Edits` files a crater into every spatial cell it can reach and `deltaAt`
	-- sums that cell's list, so the cost of one column is the number of craters
	-- that have landed near it - which climbs for as long as anybody keeps
	-- firing. Measured over 1800 ticks of the demo, with `deltaAt` stubbed out to
	-- zero: the worst tick went from 39.2 ms to 5.5 ms, and the ticks over a
	-- 60 Hz budget from 112 to none. It is not part of the rebuild cost, it is
	-- very nearly all of it.
	--
	-- **A new crater invalidates its own reach and nothing else**, which is what
	-- makes this worth having: a chunk is rebuilt because *one* crater landed in
	-- it, so all the columns the other craters dug are still good. Without that
	-- the cache would be dropped and refilled on the same frame and would save
	-- nothing.
	local realEdits = field:edits()
	local deltas: { [number]: number } = {}
	local editsProxy: any = nil

	if realEdits ~= nil then
		editsProxy = setmetatable({}, { __index = realEdits })

		function editsProxy.deltaAt(_self: any, x: number, z: number): number
			local key = columnKey(x, z)
			local held = deltas[key]
			if held == nil then
				held = realEdits:deltaAt(x, z)
				deltas[key] = held
			end
			return held
		end
	end

	-- **Substituted on the field rather than wrapped in a method**, because
	-- `HeightField` reaches for it both ways. `Chunker` asks `field:edits()`,
	-- which returns this; `HeightField.heightAt` reads `self._edits` directly,
	-- and a method override alone would have left that one path sampling the
	-- real list. That path is the one `Magic.luau`'s `groundAt` runs on every
	-- projectile every frame, so it is the one that mattered most.
	proxy._edits = editsProxy

	-- The uncached answer, for `TerrainRuntime.audit` and nothing else.
	function proxy.trueDeltaAt(x: number, z: number): number
		if realEdits == nil then
			return 0
		end
		return realEdits:deltaAt(x, z)
	end

	-- Folds a new crater into the cached depth of every column it reaches.
	--
	-- **Added rather than dropped, and that is the difference between a cache
	-- that pays and one that does not.** `Edits.deltaAt` is a plain sum over the
	-- craters filed near a column, so the depth after a crater is the depth
	-- before it plus that one crater's own contribution - which is what this
	-- asks a one-crater field for. Dropping the entries instead was correct and
	-- nearly worthless: a chunk is rebuilt *because* a crater landed in it, so
	-- every column that had just been invalidated was recomputed on the same
	-- frame, against the full pile. Measured over 1800 ticks: dropping took the
	-- ticks over a 60 Hz budget from 112 to 25, and adding takes them to none.
	--
	-- **Only columns already cached are touched.** A column nobody has sampled
	-- has no entry, and giving it this crater's contribution alone would claim
	-- the ground under every earlier crater is untouched. It stays absent and is
	-- computed in full the first time something asks.
	--
	-- The square swept is wider than the disc dug: `Edits.chunksTouchedBy`
	-- reaches `radius * 1.71 + rim * 2` and this reaches `radius * 2 + rim * 3`.
	-- Erring wide adds a zero to a few columns outside the crater; erring narrow
	-- would leave a column claiming ground that has been blown away. The
	-- constants are copied rather than read because `Edits` does not expose
	-- them, and a copy that is knowingly too big cannot drift into being too
	-- small.
	function proxy.addEdit(edit: any)
		if editsProxy == nil then
			return
		end

		-- One crater on its own, so `deltaAt` below sums a list of one whatever
		-- the real field is carrying.
		local alone = Terrain.Edits.fromRecords({ edit })

		local reach = edit.radius * 2 + (edit.rim or 0) * 3
		local minX = math.floor(edit.x - reach)
		local maxX = math.ceil(edit.x + reach)
		local minZ = math.floor(edit.z - reach)
		local maxZ = math.ceil(edit.z + reach)

		for cx = minX, maxX do
			local row = cx * 16777216
			for cz = minZ, maxZ do
				local key = row + cz
				local held = deltas[key]
				if held ~= nil then
					deltas[key] = held + alone:deltaAt(cx, cz)
				end
			end
		end
	end

	return proxy
end

function TerrainRuntime.new(field: any, size: number, container: Instance?): Builder
	local parent = container
	if parent == nil then
		parent = Instance.new("Folder")
		parent.Name = "Terrain"
		parent.Parent = workspace
	end

	return setmetatable({
		field = remembering(field),
		size = size,
		container = parent :: Instance,
		_chunks = {},
		_pool = {},
		_poolCount = 0,
		_created = 0,
		_destroyed = 0,
		_loaded = 0,
		_queue = {},
		_queued = {},
	}, TerrainRuntime) :: any
end

local function chunkKey(cx: number, cz: number): string
	return `{cx}:{cz}`
end

function TerrainRuntime.acquire(self: Builder): BasePart
	if self._poolCount > 0 then
		local part = self._pool[self._poolCount]
		self._pool[self._poolCount] = nil
		self._poolCount -= 1
		part.Visible = true
		return part
	end

	self._created += 1
	local part = Instance.new("Part")
	part.Name = "Voxels"
	part.Anchored = true
	part.Parent = self.container
	return part
end

function TerrainRuntime.release(self: Builder, part: BasePart)
	if self._poolCount >= POOL_LIMIT then
		self._destroyed += 1
		part:Destroy()
		return
	end

	-- `Visible = false` is what makes a pooled part free here: this engine's
	-- `scene::Visual::Visible` decides whether an entity is submitted at all, so
	-- an idle part costs a row and no draw. The Roblox version parks the part
	-- underground instead, because there it would still be drawn.
	part.Visible = false
	self._poolCount += 1
	self._pool[self._poolCount] = part
end

-- Builds a chunk, or rebuilds it in place if it already exists.
--
-- **A rebuild moves the old parts into their new shape rather than taking them
-- away and putting new ones back.** The two are the same boxes give or take a
-- crater's worth, so releasing the whole chunk first hid every one of them for
-- the rest of the frame and showed them again next frame: the ground under a
-- blast blinked out and returned, which reads as the terrain flickering rather
-- than as a crater appearing.
--
-- It is also less work. `release` and `acquire` each write `Visible`, and a
-- hundred parts going false and true again is two hundred property writes and
-- two hundred rows moving between archetypes, for parts that were visible
-- before and after. Only the difference in box count is pooled now.
function TerrainRuntime.build(self: Builder, cx: number, cz: number)
	local key = chunkKey(cx, cz)
	local previous = self._chunks[key]
	local reusable = if previous ~= nil then previous.count else 0

	local chunk = Chunker.build(self.field, cx, cz, self.size)

	local parts: { BasePart } = {}
	for index = 1, chunk.count do
		local box = chunk.boxes[index]
		local x, y, z, width, height, depth = Chunker.boxGeometry(box)

		-- **The old box in this slot, if there was one.** Boxes are emitted in a
		-- stable order, so slot `index` before and after a rebuild is usually
		-- the same piece of ground - but nothing depends on that being true.
		-- Whatever the part was showing, it is written in full below.
		local part = if index <= reusable then (previous :: any).parts[index] else self:acquire()

		part.Size = Vector3.new(width, height, depth)
		part.Position = Vector3.new(x, y, z)
		part.Color = COLOURS[box.surface]

		local isWater = box.surface == WATER
		part.Transparency = if isWater then WATER_TRANSPARENCY else 0
		-- A translucent shadow caster is the expensive kind and an ocean is the
		-- largest surface in the scene.
		part.CastShadow = not isWater
		part.CanCollide = not isWater

		parts[index] = part
	end

	-- Only the surplus goes back to the pool, and only once the chunk it came
	-- from is fully rebuilt. A crater usually adds boxes rather than removing
	-- them, so this is often nothing at all.
	for index = chunk.count + 1, reusable do
		self:release((previous :: any).parts[index])
	end

	self._chunks[key] = { parts = parts, count = chunk.count }
	if previous == nil then
		self._loaded += 1
	end
end

function TerrainRuntime.unload(self: Builder, cx: number, cz: number)
	local key = chunkKey(cx, cz)
	local chunk = self._chunks[key]
	if chunk == nil then
		return
	end

	for index = 1, chunk.count do
		self:release(chunk.parts[index])
	end

	self._chunks[key] = nil
	self._loaded -= 1
end

function TerrainRuntime.isLoaded(self: Builder, cx: number, cz: number): boolean
	return self._chunks[chunkKey(cx, cz)] ~= nil
end

-- Queues every loaded chunk an edit reaches for rebuilding.
--
-- **The edit decides which chunks, not a radius guess.** `Edits` knows how far a
-- crater's rim carries and this asks it, so a blast that threw ejecta across a
-- chunk boundary rebuilds both sides. Getting it wrong shows up as a crater with
-- a straight edge along a chunk seam, which reads as a rendering bug and is a
-- bookkeeping one.
--
-- **Queued rather than rebuilt here, and that is what stops the frame from
-- dropping.** A chunk rebuild re-samples the whole chunk and rebuilds every part
-- in it - measured at about 6 ms in an unoptimised build - and a crater reaches
-- up to four chunks. Several spells landing on one tick, which is what five
-- lanes on a shared cadence do, rebuilt as many as nineteen chunks inside one
-- heartbeat: a 117 ms frame against a 16.6 ms budget, which is the lag anybody
-- watching reports. Nothing about the work was wrong; doing all of it before
-- returning was.
--
-- The edit itself is already recorded - `Edits` appended it before this was
-- called - so the height field, every impact query and every subsequent crater
-- are correct the instant this returns. What waits is the *picture* of a chunk,
-- by a frame or two. See `pump`.
--
-- @return How many chunks this edit added to the queue.
function TerrainRuntime.applyEdit(self: Builder, edit: any): number
	-- **Before anything is queued**, because the cached depths for this
	-- crater's columns describe ground that no longer exists. Nothing reads them
	-- between here and the rebuild, but a queue is a delay and a stale read is
	-- easier to prevent than to find.
	self.field.addEdit(edit)

	local queued = 0
	for _, pair in Terrain.Edits.chunksTouchedBy(edit, self.size) do
		if not self:isLoaded(pair[1], pair[2]) then
			continue
		end

		-- One entry per chunk however many craters land on it. Two spells in
		-- one crater's radius used to rebuild the same chunk twice in a row,
		-- with the first result thrown away unseen.
		local key = chunkKey(pair[1], pair[2])
		if self._queued[key] then
			continue
		end

		self._queued[key] = true
		table.insert(self._queue, pair)
		queued += 1
	end
	return queued
end

-- Rebuilds at most `budget` queued chunks, oldest first.
--
-- **Called once a frame by whoever owns the beat**, which is the only place that
-- knows what a frame can afford. A budget of one keeps the worst frame to one
-- chunk whatever lands on it; raising it trades frame time for how quickly a
-- crater appears.
--
-- A chunk unloaded while it sat in the queue is dropped rather than built: it
-- will read the edit list whenever it is next loaded, which is the same
-- guarantee an unloaded chunk had before any of this.
--
-- @return How many chunks were rebuilt.
function TerrainRuntime.pump(self: Builder, budget: number): number
	local rebuilt = 0

	while rebuilt < budget do
		local pair = table.remove(self._queue, 1)
		if pair == nil then
			break
		end

		self._queued[chunkKey(pair[1], pair[2])] = nil
		if self:isLoaded(pair[1], pair[2]) then
			self:build(pair[1], pair[2])
			rebuilt += 1
		end
	end

	return rebuilt
end

-- How many chunks are waiting to be rebuilt.
function TerrainRuntime.queued(self: Builder): number
	return #self._queue
end

-- Counts columns where the cached dug depth disagrees with the real edit list.
--
-- **The guard on the one thing here that can go stale.** Every other cache in
-- this file wraps a pure function of position and can only be wrong if the noise
-- changes; the depth cache is folded forward by hand on every crater, and a
-- reach that was one stud too narrow, or an `Edits` whose contribution stopped
-- being a plain sum, would leave columns claiming ground that has been blown
-- away. That reads as a crater with a bite out of it, which looks like a mesher
-- bug and is not one.
--
-- Sampled on a stride rather than swept: the failure this catches is a whole
-- region of stale columns, never a single one, and a full sweep of the loaded
-- arena is more work than the rebuilds it is guarding.
--
-- @param stride How many columns to step between samples.
-- @return How many sampled columns disagreed.
function TerrainRuntime.audit(self: Builder, stride: number): number
	local wrong = 0
	local step = math.max(1, math.floor(stride))
	local edits = self.field:edits()
	if edits == nil then
		return 0
	end

	for key in self._chunks do
		local split = string.find(key, ":", 1, true)
		if split == nil then
			continue
		end

		local cx = tonumber(string.sub(key, 1, split - 1)) or 0
		local cz = tonumber(string.sub(key, split + 1)) or 0

		for ix = 0, self.size - 1, step do
			for iz = 0, self.size - 1, step do
				local x = cx * self.size + ix
				local z = cz * self.size + iz
				if math.abs(edits:deltaAt(x, z) - self.field.trueDeltaAt(x, z)) > 1e-4 then
					wrong += 1
				end
			end
		end
	end

	return wrong
end

-- Builds every chunk within `radius` of a chunk coordinate, nearest first.
--
-- A circle rather than a square: a square reaches 1.41 times further at its
-- corners, so with a view distance set from the axis every corner chunk is
-- built, meshed and never seen.
function TerrainRuntime.fill(self: Builder, cx: number, cz: number, radius: number): number
	local limit = radius * radius
	local built = 0

	for ring = 0, radius do
		for dz = -ring, ring do
			for dx = -ring, ring do
				if ring > 0 and math.abs(dx) ~= ring and math.abs(dz) ~= ring then
					continue
				end
				if dx * dx + dz * dz > limit then
					continue
				end
				if not self:isLoaded(cx + dx, cz + dz) then
					self:build(cx + dx, cz + dz)
					built += 1
				end
			end
		end
	end

	return built
end

function TerrainRuntime.stats(self: Builder): { [string]: number }
	local drawn = 0
	for _, chunk in self._chunks do
		drawn += chunk.count
	end
	return {
		chunks = self._loaded,
		parts = drawn,
		idle = self._poolCount,
		created = self._created,
		destroyed = self._destroyed,
	}
end

TerrainRuntime.PoolLimit = POOL_LIMIT
TerrainRuntime.Colours = COLOURS
TerrainRuntime.New = TerrainRuntime.new

return TerrainRuntime
