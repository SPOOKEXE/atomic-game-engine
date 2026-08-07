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

local TerrainCore = game:GetService("ReplicatedStorage"):FindFirstChild("TerrainCore")
assert(TerrainCore ~= nil, "ReplicatedStorage.TerrainCore is required")

local Terrain = require(TerrainCore :: any)

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
	},
	TerrainRuntime
))

local COLOURS: { Color3 } = {}
for index, surface in HeightField.Surfaces do
	COLOURS[index] = Color3.new(surface.color.r, surface.color.g, surface.color.b)
end

function TerrainRuntime.new(field: any, size: number, container: Instance?): Builder
	local parent = container
	if parent == nil then
		parent = Instance.new("Instance")
		parent.Name = "Terrain"
		parent.Parent = workspace
	end

	return setmetatable({
		field = field,
		size = size,
		container = parent :: Instance,
		_chunks = {},
		_pool = {},
		_poolCount = 0,
		_created = 0,
		_destroyed = 0,
		_loaded = 0,
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
function TerrainRuntime.build(self: Builder, cx: number, cz: number)
	if self._chunks[chunkKey(cx, cz)] ~= nil then
		self:unload(cx, cz)
	end

	local chunk = Chunker.build(self.field, cx, cz, self.size)

	local parts: { BasePart } = {}
	for index = 1, chunk.count do
		local box = chunk.boxes[index]
		local x, y, z, width, height, depth = Chunker.boxGeometry(box)

		local part = self:acquire()
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

	self._chunks[chunkKey(cx, cz)] = { parts = parts, count = chunk.count }
	self._loaded += 1
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

-- Rebuilds every loaded chunk an edit reaches.
--
-- **The edit decides which chunks, not a radius guess.** `Edits` knows how far a
-- crater's rim carries and this asks it, so a blast that threw ejecta across a
-- chunk boundary rebuilds both sides. Getting it wrong shows up as a crater with
-- a straight edge along a chunk seam, which reads as a rendering bug and is a
-- bookkeeping one.
function TerrainRuntime.applyEdit(self: Builder, edit: any): number
	local rebuilt = 0
	for _, pair in Terrain.Edits.chunksTouchedBy(edit, self.size) do
		if self:isLoaded(pair[1], pair[2]) then
			self:build(pair[1], pair[2])
			rebuilt += 1
		end
	end
	return rebuilt
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
