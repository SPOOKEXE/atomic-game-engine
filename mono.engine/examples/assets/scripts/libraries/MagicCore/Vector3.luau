--!strict

export type Vector = {
	x: number,
	y: number,
	z: number,
}

local Vector3 = {}

local function new(x: number, y: number, z: number): Vector
	return { x = x, y = y, z = z }
end

function Vector3.new(x: number, y: number, z: number): Vector
	return new(x, y, z)
end

function Vector3.zero(): Vector
	return new(0, 0, 0)
end

function Vector3.clone(value: Vector): Vector
	return new(value.x, value.y, value.z)
end

function Vector3.add(a: Vector, b: Vector): Vector
	return new(a.x + b.x, a.y + b.y, a.z + b.z)
end

function Vector3.sub(a: Vector, b: Vector): Vector
	return new(a.x - b.x, a.y - b.y, a.z - b.z)
end

function Vector3.scale(value: Vector, scalar: number): Vector
	return new(value.x * scalar, value.y * scalar, value.z * scalar)
end

function Vector3.dot(a: Vector, b: Vector): number
	return a.x * b.x + a.y * b.y + a.z * b.z
end

function Vector3.cross(a: Vector, b: Vector): Vector
	return new(
		a.y * b.z - a.z * b.y,
		a.z * b.x - a.x * b.z,
		a.x * b.y - a.y * b.x
	)
end

function Vector3.magnitudeSquared(value: Vector): number
	return Vector3.dot(value, value)
end

function Vector3.magnitude(value: Vector): number
	return math.sqrt(Vector3.magnitudeSquared(value))
end

function Vector3.unit(value: Vector): Vector
	local length = Vector3.magnitude(value)
	if length == 0 then
		return Vector3.zero()
	end
	return Vector3.scale(value, 1 / length)
end

function Vector3.lerp(a: Vector, b: Vector, alpha: number): Vector
	return new(
		a.x + (b.x - a.x) * alpha,
		a.y + (b.y - a.y) * alpha,
		a.z + (b.z - a.z) * alpha
	)
end

function Vector3.distance(a: Vector, b: Vector): number
	return Vector3.magnitude(Vector3.sub(a, b))
end

function Vector3.isFinite(value: any): boolean
	if type(value) ~= "table" then
		return false
	end
	local x = value.x
	local y = value.y
	local z = value.z
	return type(x) == "number"
		and x == x
		and math.abs(x) ~= math.huge
		and type(y) == "number"
		and y == y
		and math.abs(y) ~= math.huge
		and type(z) == "number"
		and z == z
		and math.abs(z) ~= math.huge
end

Vector3.New = Vector3.new
Vector3.Add = Vector3.add
Vector3.Sub = Vector3.sub
Vector3.Scale = Vector3.scale
Vector3.Dot = Vector3.dot
Vector3.Cross = Vector3.cross
Vector3.Magnitude = Vector3.magnitude
Vector3.Unit = Vector3.unit
Vector3.Lerp = Vector3.lerp

return table.freeze(Vector3)
