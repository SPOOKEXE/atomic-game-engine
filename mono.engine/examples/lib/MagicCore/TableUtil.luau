--!strict

local TableUtil = {}

function TableUtil.deepCopy<T>(value: T): T
	local seen: { [any]: any } = {}

	local function copy(input: any): any
		if type(input) ~= "table" then
			return input
		end

		local prior = seen[input]
		if prior ~= nil then
			return prior
		end

		local output = {}
		seen[input] = output
		for key, child in pairs(input) do
			output[copy(key)] = copy(child)
		end
		return output
	end

	return copy(value) :: T
end

-- Idempotent by design: `table.freeze` throws on an already-frozen table, but an
-- already-frozen subtree already satisfies what this function promises. Compiler
-- outputs are composed from previously frozen documents, so the frozen case is normal
-- rather than exceptional. Children are still visited, because a frozen table may hold
-- a mutable one.
function TableUtil.deepFreeze<T>(value: T): T
	local seen: { [any]: boolean } = {}

	local function freeze(input: any)
		if type(input) ~= "table" or seen[input] then
			return
		end

		seen[input] = true
		for key, child in pairs(input) do
			freeze(key)
			freeze(child)
		end
		if not table.isfrozen(input) then
			table.freeze(input)
		end
	end

	freeze(value)
	return value
end

-- Returns nil for sparse, mixed-key, zero-indexed, or non-integer-key tables.
function TableUtil.denseArrayLength(value: any): number?
	if type(value) ~= "table" then
		return nil
	end

	local count = 0
	local maximum = 0
	for key in pairs(value) do
		if type(key) ~= "number" or key < 1 or key % 1 ~= 0 then
			return nil
		end
		count += 1
		if key > maximum then
			maximum = key
		end
	end

	if maximum ~= count then
		return nil
	end
	return count
end

function TableUtil.sortedStringKeys(value: { [string]: any }): { string }
	local keys = {}
	for key in pairs(value) do
		table.insert(keys, key)
	end
	table.sort(keys)
	return keys
end

function TableUtil.shallowAssign(target: { [any]: any }, source: { [any]: any }): { [any]: any }
	for key, value in pairs(source) do
		target[key] = value
	end
	return target
end

TableUtil.DeepCopy = TableUtil.deepCopy
TableUtil.DeepFreeze = TableUtil.deepFreeze
TableUtil.DenseArrayLength = TableUtil.denseArrayLength

return table.freeze(TableUtil)
