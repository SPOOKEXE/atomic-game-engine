--!strict

local SpatialIndex = {}

export type Actor = {
	Id: number,
	Position: Vector3,
	Faction: string,
	Alive: boolean,
}

type Cell = { Actor }
type Column = { [number]: Cell }

export type Grid = {
	CellSize: number,
	Columns: { [number]: Column },
}

local function coordinate(value: number, cellSize: number): number
	return math.floor(value / cellSize)
end

function SpatialIndex.build(actors: { Actor }, cellSize: number): Grid
	assert(cellSize > 0, "cell size must be positive")
	local columns: { [number]: Column } = {}
	for _, actor in actors do
		if actor.Alive then
			local x = coordinate(actor.Position.X, cellSize)
			local z = coordinate(actor.Position.Z, cellSize)
			local column = columns[x]
			if column == nil then
				column = {}
				columns[x] = column
			end
			local cell = column[z]
			if cell == nil then
				cell = {}
				column[z] = cell
			end
			table.insert(cell, actor)
		end
	end
	return { CellSize = cellSize, Columns = columns }
end

local function nearest(
	grid: Grid,
	origin: Vector3,
	faction: string,
	actorId: number?,
	maximumDistance: number,
	forward: Vector3?
): Actor?
	if maximumDistance <= 0 then
		return nil
	end
	local direction = nil
	if forward ~= nil then
		if forward.Magnitude <= 0 then
			return nil
		end
		direction = forward.Unit
	end

	local centreX = coordinate(origin.X, grid.CellSize)
	local centreZ = coordinate(origin.Z, grid.CellSize)
	local radius = math.ceil(maximumDistance / grid.CellSize)
	local found = nil
	local foundDistance = math.huge
	for x = centreX - radius, centreX + radius do
		local column = grid.Columns[x]
		if column == nil then
			continue
		end
		for z = centreZ - radius, centreZ + radius do
			local cell = column[z]
			if cell == nil then
				continue
			end
			for _, candidate in cell do
				if candidate.Faction == faction or candidate.Id == actorId then
					continue
				end
				local offset = candidate.Position - origin
				local distance = offset.Magnitude
				if distance <= 0 or distance > maximumDistance then
					continue
				end
				if direction ~= nil and offset:Dot(direction) / distance < 0.35 then
					continue
				end
				local winsTie = distance == foundDistance and found ~= nil and candidate.Id < found.Id
				if distance < foundDistance or winsTie then
					found = candidate
					foundDistance = distance
				end
			end
		end
	end
	return found
end

function SpatialIndex.nearestEnemy(grid: Grid, actor: Actor, maximumDistance: number): Actor?
	return nearest(grid, actor.Position, actor.Faction, actor.Id, maximumDistance, nil)
end

function SpatialIndex.nearestTarget(
	grid: Grid,
	origin: Vector3,
	forward: Vector3,
	faction: string,
	maximumDistance: number
): Actor?
	return nearest(grid, origin, faction, nil, maximumDistance, forward)
end

return SpatialIndex
