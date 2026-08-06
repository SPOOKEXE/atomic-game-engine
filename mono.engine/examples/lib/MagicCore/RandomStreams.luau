--!strict

-- Stable seed format, version 1:
--   * root and numeric keys are signed safe integers encoded little-endian;
--   * strings are length-prefixed bytes;
--   * booleans, numbers, and strings have distinct one-byte tags;
--   * bytes are folded with 32-bit FNV-1a, then a fixed Murmur-style avalanche;
--   * the result is reduced to [0, 2147483646] for Random.new.
-- Do not change this encoding without a stream-format version bump and vectors.

export type StablePrimitive = string | number | boolean
export type StableKey = StablePrimitive | {
	type: "string" | "number" | "boolean",
	value: StablePrimitive,
}

local UINT32 = 4294967296
local UINT16 = 65536
local MAX_SAFE_INTEGER = 9007199254740991
local RANDOM_SEED_MODULUS = 2147483647
local MAX_KEY_BYTES = 1024

local function multiply32(a: number, b: number): number
	local aLow = bit32.band(a, 0xFFFF)
	local aHigh = bit32.rshift(a, 16)
	local bLow = bit32.band(b, 0xFFFF)
	local bHigh = bit32.rshift(b, 16)
	local lowProduct = aLow * bLow
	local cross = aLow * bHigh + aHigh * bLow
	return bit32.band(lowProduct + (cross % UINT16) * UINT16, 0xFFFFFFFF)
end

local function writeByte(state: number, byte: number): number
	return multiply32(bit32.bxor(state, byte), 16777619)
end

local function writeUnsigned32(state: number, value: number): number
	local remaining = value % UINT32
	for _ = 1, 4 do
		state = writeByte(state, remaining % 256)
		remaining = math.floor(remaining / 256)
	end
	return state
end

local function isFiniteInteger(value: any): boolean
	return type(value) == "number"
		and value == value
		and math.abs(value) ~= math.huge
		and value % 1 == 0
		and math.abs(value) <= MAX_SAFE_INTEGER
end

local function writeSignedInteger(state: number, value: number): number
	state = writeByte(state, if value < 0 then 1 else 0)
	local magnitude = math.abs(value)
	local low = magnitude % UINT32
	local high = math.floor(magnitude / UINT32)
	state = writeUnsigned32(state, low)
	return writeUnsigned32(state, high)
end

local function writeString(state: number, value: string): number
	if #value > MAX_KEY_BYTES then
		error("stable string key exceeds 1024 bytes", 3)
	end
	state = writeUnsigned32(state, #value)
	for index = 1, #value do
		state = writeByte(state, string.byte(value, index))
	end
	return state
end

local function unpackStableKey(key: any): (string, any)
	local primitiveType = type(key)
	if primitiveType == "string" or primitiveType == "number" or primitiveType == "boolean" then
		return primitiveType, key
	end

	if primitiveType ~= "table" then
		error("stable keys must be strings, safe integers, booleans, or typed key records", 3)
	end
	for field in pairs(key) do
		if field ~= "type" and field ~= "value" then
			error("typed stable key contains an unknown field", 3)
		end
	end
	local declaredType = key.type
	if declaredType ~= "string" and declaredType ~= "number" and declaredType ~= "boolean" then
		error("typed stable key has an unsupported type", 3)
	end
	if type(key.value) ~= declaredType then
		error("typed stable key value does not match its type", 3)
	end
	return declaredType, key.value
end

local function writeStableKey(state: number, key: any): number
	local keyType, value = unpackStableKey(key)
	if keyType == "string" then
		state = writeByte(state, 0x53)
		return writeString(state, value)
	elseif keyType == "number" then
		if not isFiniteInteger(value) then
			error("numeric stable keys must be finite safe integers", 3)
		end
		state = writeByte(state, 0x4E)
		return writeSignedInteger(state, value)
	else
		state = writeByte(state, 0x42)
		return writeByte(state, if value then 1 else 0)
	end
end

local function avalanche(state: number): number
	state = bit32.bxor(state, bit32.rshift(state, 16))
	state = multiply32(state, 0x85EBCA6B)
	state = bit32.bxor(state, bit32.rshift(state, 13))
	state = multiply32(state, 0xC2B2AE35)
	return bit32.bxor(state, bit32.rshift(state, 16))
end

local RandomStreams = {}

function RandomStreams.deriveSeed(rootSeed: number, ...: StableKey): number
	if not isFiniteInteger(rootSeed) then
		error("root seed must be a finite safe integer", 2)
	end

	local state = 2166136261
	state = writeByte(state, 0x52)
	state = writeSignedInteger(state, rootSeed)
	local keyCount = select("#", ...)
	for index = 1, keyCount do
		local key = select(index, ...)
		state = writeStableKey(state, key)
	end
	state = avalanche(state)
	return state % RANDOM_SEED_MODULUS
end

function RandomStreams.new(rootSeed: number, ...: StableKey): any
	return Random.new(RandomStreams.deriveSeed(rootSeed, ...))
end

function RandomStreams.typed(keyType: "string" | "number" | "boolean", value: StablePrimitive): StableKey
	return {
		type = keyType,
		value = value,
	}
end

RandomStreams.DeriveSeed = RandomStreams.deriveSeed
RandomStreams.New = RandomStreams.new
RandomStreams.Stream = RandomStreams.new
RandomStreams.Typed = RandomStreams.typed
RandomStreams.FORMAT_VERSION = 1
RandomStreams.MIN_DERIVED_SEED = 0
RandomStreams.MAX_DERIVED_SEED = RANDOM_SEED_MODULUS - 1

return table.freeze(RandomStreams)
