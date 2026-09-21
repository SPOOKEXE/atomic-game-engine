--!strict

local TableUtil = require(script.Parent.TableUtil)
local Types = require(script.Parent.Types)
local Enums = require(script.Parent.Enums)

type Diagnostic = {
	code: string,
	path: string,
	message: string,
	severity: "Info" | "Warning" | "Error",
}

type Context = {
	diagnostics: { Diagnostic },
	nonfinitePaths: { [string]: boolean },
}

local Limits = Types.Limits
local Validator = {}

local function addDiagnostic(
	context: Context,
	code: string,
	path: string,
	message: string,
	severity: ("Info" | "Warning" | "Error")?
)
	local resolvedSeverity: "Info" | "Warning" | "Error" = if severity == nil then "Error" else severity
	table.insert(context.diagnostics, {
		code = code,
		path = path,
		message = message,
		severity = resolvedSeverity,
	})
end

local function childPath(path: string, key: any): string
	if type(key) == "number" then
		return path .. "[" .. tostring(key) .. "]"
	elseif type(key) == "string" then
		return path .. "." .. key
	end
	return path .. ".<invalid-key>"
end

local function sortedKeys(value: { [any]: any }): { any }
	local keys = {}
	for key in next, value do
		table.insert(keys, key)
	end
	table.sort(keys, function(a: any, b: any): boolean
		local aType = type(a)
		local bType = type(b)
		if aType ~= bType then
			return aType < bType
		end
		if aType == "number" or aType == "string" then
			return a < b
		end
		return tostring(a) < tostring(b)
	end)
	return keys
end

local function isFinite(value: any): boolean
	return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end

local function isValidUtf8(value: string): boolean
	return utf8.len(value) ~= nil
end

local function scanPureData(context: Context, root: any): boolean
	local seen: { [any]: string } = {}
	local valueCount = 0
	local aborted = false

	local function abort(code: string, path: string, message: string)
		if aborted then
			return
		end
		aborted = true
		addDiagnostic(context, code, path, message)
	end

	local function scan(value: any, path: string, depth: number)
		if aborted then
			return
		end
		if depth > Limits.MAX_DOCUMENT_DEPTH then
			abort("DOCUMENT_TOO_DEEP", path, "Document exceeds the nesting-depth cap.")
			return
		end
		if valueCount >= Limits.MAX_DOCUMENT_VALUES then
			abort("DOCUMENT_TOO_LARGE", path, "Document exceeds the value-count cap.")
			return
		end
		valueCount += 1

		local valueType = typeof(value)
		if valueType == "number" then
			if not isFinite(value) then
				context.nonfinitePaths[path] = true
				addDiagnostic(context, "NONFINITE_NUMBER", path, "Numbers must be finite.")
			end
			return
		elseif valueType == "string" then
			if #value > Limits.MAX_STRING_LENGTH then
				addDiagnostic(context, "STRING_TOO_LONG", path, "String exceeds the supported byte length.")
			end
			if not isValidUtf8(value) then
				addDiagnostic(context, "INVALID_UTF8", path, "Strings must contain valid UTF-8.")
			end
			return
		elseif valueType == "boolean" then
			return
		elseif valueType ~= "table" then
			addDiagnostic(
				context,
				"UNSAFE_VALUE",
				path,
				"Spell data cannot contain " .. valueType .. " values; only JSON-safe data is supported."
			)
			return
		end

		if getmetatable(value) ~= nil then
			abort("UNSAFE_METATABLE", path, "Spell data tables cannot have metatables.")
			return
		end
		local firstPath = seen[value]
		if firstPath ~= nil then
			addDiagnostic(
				context,
				"SHARED_OR_CYCLIC_TABLE",
				path,
				"Spell data must be a tree; this table was already seen at " .. firstPath .. "."
			)
			return
		end
		seen[value] = path

		for key, child in next, value do
			if aborted then
				return
			end
			local keyType = type(key)
			local nextPath = path .. ".<invalid-key>"
			if keyType == "number" then
				if key < 1 or key % 1 ~= 0 then
					addDiagnostic(context, "INVALID_TABLE_KEY", path, "Array keys must be positive integers.")
				else
					nextPath = childPath(path, key)
				end
			elseif keyType == "string" then
				local keyLengthValid = #key <= Limits.MAX_STRING_LENGTH
				local keyEncodingValid = isValidUtf8(key)
				if not keyLengthValid then
					addDiagnostic(context, "STRING_TOO_LONG", nextPath, "Map key exceeds the supported byte length.")
				end
				if not keyEncodingValid then
					addDiagnostic(context, "INVALID_UTF8", nextPath, "Map keys must contain valid UTF-8.")
				end
				if keyLengthValid and keyEncodingValid then
					nextPath = childPath(path, key)
				end
			else
				addDiagnostic(context, "INVALID_TABLE_KEY", path, "Map keys must be strings.")
			end
			scan(child, nextPath, depth + 1)
		end
	end

	scan(root, "$", 1)
	return not aborted
end

local function expectTable(context: Context, value: any, path: string): { [any]: any }?
	if type(value) ~= "table" then
		addDiagnostic(context, "TYPE_MISMATCH", path, "Expected a table.")
		return nil
	end
	return value
end

local function checkFields(
	context: Context,
	value: { [any]: any },
	path: string,
	allowed: { [string]: boolean },
	knownDeferred: { [string]: string }?
)
	for _, key in ipairs(sortedKeys(value)) do
		if type(key) ~= "string" then
			addDiagnostic(context, "UNKNOWN_FIELD", childPath(path, key), "This record accepts string fields only.")
		elseif not allowed[key] then
			local deferredMessage = if knownDeferred then knownDeferred[key] else nil
			if deferredMessage ~= nil then
				addDiagnostic(context, "UNSUPPORTED_FIELD", childPath(path, key), deferredMessage)
			else
				addDiagnostic(context, "UNKNOWN_FIELD", childPath(path, key), "Unknown field '" .. key .. "'.")
			end
		end
	end
end

local function requireField(context: Context, value: { [any]: any }, field: string, path: string): any
	local result = value[field]
	if result == nil then
		addDiagnostic(context, "MISSING_FIELD", path .. "." .. field, "Required field is missing.")
	end
	return result
end

local function checkNumber(
	context: Context,
	value: any,
	path: string,
	minimum: number?,
	maximum: number?,
	integer: boolean?
): boolean
	if type(value) ~= "number" then
		addDiagnostic(context, "TYPE_MISMATCH", path, "Expected a number.")
		return false
	end
	if not isFinite(value) then
		if not context.nonfinitePaths[path] then
			addDiagnostic(context, "NONFINITE_NUMBER", path, "Numbers must be finite.")
		end
		return false
	end
	if integer and value % 1 ~= 0 then
		addDiagnostic(context, "INTEGER_REQUIRED", path, "Expected an integer.")
		return false
	end
	if minimum ~= nil and value < minimum then
		addDiagnostic(context, "OUT_OF_BOUNDS", path, string.format("Value must be at least %g.", minimum))
		return false
	end
	if maximum ~= nil and value > maximum then
		addDiagnostic(context, "OUT_OF_BOUNDS", path, string.format("Value must be at most %g.", maximum))
		return false
	end
	return true
end

local function checkString(context: Context, value: any, path: string): boolean
	if type(value) ~= "string" then
		addDiagnostic(context, "TYPE_MISMATCH", path, "Expected a string.")
		return false
	end
	if value == "" then
		addDiagnostic(context, "EMPTY_STRING", path, "String cannot be empty.")
		return false
	end
	return true
end

local function checkId(context: Context, value: any, path: string): boolean
	if not checkString(context, value, path) then
		return false
	end
	if #value > Limits.MAX_ID_LENGTH then
		addDiagnostic(context, "ID_TOO_LONG", path, "ID exceeds the supported byte length.")
		return false
	end
	if string.match(value, "^[A-Za-z][A-Za-z0-9_.%-]*$") == nil then
		addDiagnostic(context, "INVALID_ID", path, "ID must start with a letter and use letters, digits, '.', '_', or '-'.")
		return false
	end
	return true
end

local function checkEnum(context: Context, value: any, path: string, allowed: { [string]: boolean }): boolean
	if type(value) ~= "string" then
		addDiagnostic(context, "TYPE_MISMATCH", path, "Expected a string enum value.")
		return false
	end
	if not allowed[value] then
		addDiagnostic(context, "INVALID_ENUM", path, "Unsupported value '" .. value .. "'.")
		return false
	end
	return true
end

local function checkBoolean(context: Context, value: any, path: string): boolean
	if type(value) ~= "boolean" then
		addDiagnostic(context, "TYPE_MISMATCH", path, "Expected a boolean.")
		return false
	end
	return true
end

local function checkArray(context: Context, value: any, path: string, maximum: number): (number?, { [any]: any }?)
	local array = expectTable(context, value, path)
	if array == nil then
		return nil, nil
	end
	local length = TableUtil.denseArrayLength(array)
	if length == nil then
		addDiagnostic(context, "SPARSE_ARRAY", path, "Expected a dense one-indexed array with no map fields.")
		return nil, array
	end
	if length > maximum then
		addDiagnostic(context, "ARRAY_TOO_LARGE", path, string.format("Array may contain at most %d values.", maximum))
	end
	return length, array
end

local function checkTagArray(context: Context, value: any, path: string): { [string]: boolean }
	local tags: { [string]: boolean } = {}
	local length, array = checkArray(context, value, path, Limits.MAX_TAGS)
	if length == nil or array == nil then
		return tags
	end
	for index = 1, length do
		local tagPath = string.format("%s[%d]", path, index)
		local tag = array[index]
		if checkId(context, tag, tagPath) then
			if tags[tag] then
				addDiagnostic(context, "DUPLICATE_TAG", tagPath, "Tags in one list must be unique.")
			else
				tags[tag] = true
			end
		end
	end
	return tags
end

local function checkVector(context: Context, value: any, path: string): boolean
	local vector = expectTable(context, value, path)
	if vector == nil then
		return false
	end
	checkFields(context, vector, path, { x = true, y = true, z = true })
	local x = requireField(context, vector, "x", path)
	local y = requireField(context, vector, "y", path)
	local z = requireField(context, vector, "z", path)
	local xOk = checkNumber(context, x, path .. ".x", -Limits.MAX_VECTOR_COMPONENT, Limits.MAX_VECTOR_COMPONENT)
	local yOk = checkNumber(context, y, path .. ".y", -Limits.MAX_VECTOR_COMPONENT, Limits.MAX_VECTOR_COMPONENT)
	local zOk = checkNumber(context, z, path .. ".z", -Limits.MAX_VECTOR_COMPONENT, Limits.MAX_VECTOR_COMPONENT)
	return xOk and yOk and zOk
end

local function vectorMagnitude(value: { [any]: any }): number
	return math.sqrt(value.x * value.x + value.y * value.y + value.z * value.z)
end

local function validateLifetime(context: Context, value: any): number?
	local path = "$.projectile.lifetime"
	local lifetime = expectTable(context, value, path)
	if lifetime == nil then
		return nil
	end
	checkFields(context, lifetime, path, { kind = true, seconds = true })
	local kind = requireField(context, lifetime, "kind", path)
	if kind ~= nil and kind ~= "Lifetime.Time" then
		addDiagnostic(context, "UNSUPPORTED_KIND", path .. ".kind", "Only Lifetime.Time is supported in schema version 1.")
	end
	local seconds = requireField(context, lifetime, "seconds", path)
	if checkNumber(context, seconds, path .. ".seconds", 0.001, Limits.MAX_LIFETIME_SECONDS) then
		return seconds
	end
	return nil
end

local function validateMotion(context: Context, value: any, lifetimeSeconds: number?)
	local path = "$.projectile.motion"
	local motion = expectTable(context, value, path)
	if motion == nil then
		return
	end
	local kind = requireField(context, motion, "kind", path)
	if kind == "Motion.Linear" then
		checkFields(context, motion, path, { kind = true, velocity = true })
		local velocity = requireField(context, motion, "velocity", path)
		if checkVector(context, velocity, path .. ".velocity") then
			local speed = vectorMagnitude(velocity)
			if speed <= 0 or speed > Limits.MAX_SPEED then
				addDiagnostic(context, "OUT_OF_BOUNDS", path .. ".velocity", "Linear speed must be greater than zero and within the speed cap.")
			end
		end
	elseif kind == "Motion.Ballistic" then
		checkFields(context, motion, path, { kind = true, velocity = true, acceleration = true })
		local velocity = requireField(context, motion, "velocity", path)
		local acceleration = requireField(context, motion, "acceleration", path)
		if checkVector(context, velocity, path .. ".velocity") and vectorMagnitude(velocity) > Limits.MAX_SPEED then
			addDiagnostic(context, "OUT_OF_BOUNDS", path .. ".velocity", "Ballistic velocity exceeds the speed cap.")
		end
		if checkVector(context, acceleration, path .. ".acceleration")
			and vectorMagnitude(acceleration) > Limits.MAX_ACCELERATION
		then
			addDiagnostic(context, "OUT_OF_BOUNDS", path .. ".acceleration", "Ballistic acceleration exceeds the acceleration cap.")
		end
	elseif kind == "Motion.Bezier" then
		checkFields(context, motion, path, { kind = true, duration = true, space = true, points = true }, {
			tracking = "Bezier tracking is deferred; control points are fixed launch/world snapshots in this slice.",
		})
		local duration = requireField(context, motion, "duration", path)
		if checkNumber(context, duration, path .. ".duration", 0.001, Limits.MAX_LIFETIME_SECONDS)
			and lifetimeSeconds ~= nil
			and duration > lifetimeSeconds
		then
			addDiagnostic(context, "OUT_OF_BOUNDS", path .. ".duration", "Bezier duration cannot exceed projectile lifetime.")
		end
		checkEnum(context, requireField(context, motion, "space", path), path .. ".space", {
			LaunchLocal = true,
			WorldSnapshot = true,
		})
		local length, points = checkArray(context, requireField(context, motion, "points", path), path .. ".points", 4)
		if length ~= nil and length ~= 4 then
			addDiagnostic(context, "INVALID_POINT_COUNT", path .. ".points", "Cubic Bezier motion requires exactly four points.")
		end
		if points ~= nil and length ~= nil then
			for index = 1, length do
				checkVector(context, points[index], string.format("%s.points[%d]", path, index))
			end
		end
	else
		checkFields(context, motion, path, { kind = true })
		if kind ~= nil then
			addDiagnostic(
				context,
				"UNSUPPORTED_KIND",
				path .. ".kind",
				"Supported motion kinds are Motion.Linear, Motion.Ballistic, and Motion.Bezier; tracking is deferred."
			)
		end
	end
end

local function validateColor(context: Context, value: any, path: string)
	local color = expectTable(context, value, path)
	if color == nil then
		return
	end
	checkFields(context, color, path, { r = true, g = true, b = true })
	checkNumber(context, requireField(context, color, "r", path), path .. ".r", 0, 1)
	checkNumber(context, requireField(context, color, "g", path), path .. ".g", 0, 1)
	checkNumber(context, requireField(context, color, "b", path), path .. ".b", 0, 1)
end

local function validateRange(context: Context, value: any, path: string): number?
	local range = expectTable(context, value, path)
	if range == nil then
		return nil
	end
	checkFields(context, range, path, { min = true, max = true })
	local minimum = requireField(context, range, "min", path)
	local maximum = requireField(context, range, "max", path)
	local minOk = checkNumber(context, minimum, path .. ".min", 0, Limits.MAX_PARTICLE_LIFETIME)
	local maxOk = checkNumber(context, maximum, path .. ".max", 0, Limits.MAX_PARTICLE_LIFETIME)
	if minOk and maxOk then
		if minimum > maximum then
			addDiagnostic(context, "INVALID_RANGE", path, "Range min cannot exceed max.")
		end
		return maximum
	end
	return nil
end

local function validateParticles(
	context: Context,
	value: any,
	lifetimeSeconds: number?
): ({ [string]: boolean }, number, number)
	local emitterIds: { [string]: boolean } = {}
	local totalRateCost = 0
	local maxParticleLifetime = 0
	local length, particles = checkArray(context, value, "$.projectile.presentation.particles", Limits.MAX_PARTICLES)
	if length == nil or particles == nil then
		return emitterIds, totalRateCost, maxParticleLifetime
	end

	for index = 1, length do
		local path = string.format("$.projectile.presentation.particles[%d]", index)
		local emitter = expectTable(context, particles[index], path)
		if emitter ~= nil then
			checkFields(context, emitter, path, {
				kind = true,
				id = true,
				enabledSeconds = true,
				lifetime = true,
				rate = true,
			}, {
				speed = "Particle speed authoring is deferred in the first presentation slice.",
				acceleration = "Particle acceleration authoring is deferred in the first presentation slice.",
				spread = "Particle spread authoring is deferred in the first presentation slice.",
			})
			local kind = requireField(context, emitter, "kind", path)
			if kind ~= nil and kind ~= "Presentation.Particle" then
				addDiagnostic(context, "UNSUPPORTED_KIND", path .. ".kind", "Only Presentation.Particle emitters are supported.")
			end
			local id = requireField(context, emitter, "id", path)
			if checkId(context, id, path .. ".id") then
				if emitterIds[id] then
					addDiagnostic(context, "DUPLICATE_EMITTER_ID", path .. ".id", "Particle emitter IDs must be unique.")
				else
					emitterIds[id] = true
				end
			end
			local enabledSeconds = requireField(context, emitter, "enabledSeconds", path)
			if checkNumber(context, enabledSeconds, path .. ".enabledSeconds", 0, Limits.MAX_LIFETIME_SECONDS) then
				if lifetimeSeconds ~= nil and enabledSeconds > lifetimeSeconds then
					addDiagnostic(context, "OUT_OF_BOUNDS", path .. ".enabledSeconds", "Emitter enable time cannot exceed owner lifetime in this slice.")
				end
			end
			local particleLifetime = validateRange(context, requireField(context, emitter, "lifetime", path), path .. ".lifetime")
			if particleLifetime ~= nil then
				maxParticleLifetime = math.max(maxParticleLifetime, particleLifetime)
			end
			local rate = if emitter.rate == nil then 0 else emitter.rate
			if checkNumber(context, rate, path .. ".rate", 0, Limits.MAX_PARTICLE_RATE) and isFinite(enabledSeconds) then
				totalRateCost += math.ceil(rate * enabledSeconds)
			end
		end
	end
	return emitterIds, totalRateCost, maxParticleLifetime
end

local function validateCurves(context: Context, value: any, lifetimeSeconds: number?)
	local length, curves = checkArray(context, value, "$.projectile.presentation.curves", Limits.MAX_CURVES)
	if length == nil or curves == nil then
		return
	end
	local curveIds: { [string]: boolean } = {}
	for index = 1, length do
		local path = string.format("$.projectile.presentation.curves[%d]", index)
		local curve = expectTable(context, curves[index], path)
		if curve ~= nil then
			checkFields(context, curve, path, {
				kind = true,
				id = true,
				property = true,
				timeMode = true,
				keys = true,
			})
			local kind = requireField(context, curve, "kind", path)
			if kind ~= nil and kind ~= "Presentation.Curve" then
				addDiagnostic(context, "UNSUPPORTED_KIND", path .. ".kind", "Only Presentation.Curve is supported.")
			end
			local id = requireField(context, curve, "id", path)
			if checkId(context, id, path .. ".id") then
				if curveIds[id] then
					addDiagnostic(context, "DUPLICATE_CURVE_ID", path .. ".id", "Curve IDs must be unique.")
				else
					curveIds[id] = true
				end
			end
			local property = requireField(context, curve, "property", path)
			checkEnum(context, property, path .. ".property", { Scale = true, Transparency = true })
			local timeMode = requireField(context, curve, "timeMode", path)
			checkEnum(context, timeMode, path .. ".timeMode", {
				NormalizedLifetime = true,
				AbsoluteSeconds = true,
			})
			local keyCount, keys = checkArray(context, requireField(context, curve, "keys", path), path .. ".keys", Limits.MAX_CURVE_KEYS)
			if keyCount ~= nil and keyCount < 2 then
				addDiagnostic(context, "CURVE_TOO_SHORT", path .. ".keys", "A curve requires at least two keys.")
			end
			if keyCount ~= nil and keys ~= nil then
				local priorTime: number? = nil
				for keyIndex = 1, keyCount do
					local keyPath = string.format("%s.keys[%d]", path, keyIndex)
					local key = expectTable(context, keys[keyIndex], keyPath)
					if key ~= nil then
						checkFields(context, key, keyPath, { time = true, value = true })
						local keyTime = requireField(context, key, "time", keyPath)
						local maxTime = if timeMode == "NormalizedLifetime" then 1 else lifetimeSeconds or Limits.MAX_LIFETIME_SECONDS
						if checkNumber(context, keyTime, keyPath .. ".time", 0, maxTime) then
							if priorTime ~= nil and keyTime <= priorTime then
								addDiagnostic(context, "INVALID_CURVE_ORDER", keyPath .. ".time", "Curve key times must be strictly increasing.")
							end
							priorTime = keyTime
						end
						local keyValue = requireField(context, key, "value", keyPath)
						if property == "Transparency" then
							checkNumber(context, keyValue, keyPath .. ".value", 0, 1)
						else
							checkNumber(context, keyValue, keyPath .. ".value", 0, 100)
						end
					end
				end
			end
		end
	end
end

local function validateTimeline(
	context: Context,
	value: any,
	emitterIds: { [string]: boolean }
): (number, number)
	local length, timeline = checkArray(context, value, "$.projectile.presentation.timeline", Limits.MAX_TIMELINE_CUES)
	if length == nil or timeline == nil then
		return 0, 0
	end
	local cueIds: { [string]: boolean } = {}
	local priorTime: number? = nil
	local emittedCount = 0
	local latestTime = 0
	for index = 1, length do
		local path = string.format("$.projectile.presentation.timeline[%d]", index)
		local cue = expectTable(context, timeline[index], path)
		if cue ~= nil then
			local kind = requireField(context, cue, "kind", path)
			if kind == "Timeline.EmitParticles" then
				checkFields(context, cue, path, {
					kind = true,
					id = true,
					time = true,
					emitterId = true,
					count = true,
				})
				local count = requireField(context, cue, "count", path)
				if checkNumber(context, count, path .. ".count", 1, Limits.MAX_PARTICLE_BUDGET, true) then
					emittedCount += count
				end
			elseif kind == "Timeline.SetEmitterEnabled" then
				checkFields(context, cue, path, {
					kind = true,
					id = true,
					time = true,
					emitterId = true,
					enabled = true,
				})
				local enabled = requireField(context, cue, "enabled", path)
				if checkBoolean(context, enabled, path .. ".enabled") and enabled then
					addDiagnostic(
						context,
						"EMITTER_REENABLE_UNSUPPORTED",
						path .. ".enabled",
						"Timeline.SetEmitterEnabled supports disable-only cues; enabled must be false."
					)
				end
			else
				checkFields(context, cue, path, { kind = true })
				if kind ~= nil then
					addDiagnostic(context, "UNSUPPORTED_KIND", path .. ".kind", "Unsupported timeline cue kind.")
				end
			end

			local id = requireField(context, cue, "id", path)
			if checkId(context, id, path .. ".id") then
				if cueIds[id] then
					addDiagnostic(context, "DUPLICATE_CUE_ID", path .. ".id", "Timeline cue IDs must be unique.")
				else
					cueIds[id] = true
				end
			end
			local cueTime = requireField(context, cue, "time", path)
			if checkNumber(context, cueTime, path .. ".time", 0, Limits.MAX_LIFETIME_SECONDS) then
				if priorTime ~= nil and cueTime < priorTime then
					addDiagnostic(context, "INVALID_TIMELINE_ORDER", path .. ".time", "Timeline cue times must be nondecreasing.")
				end
				priorTime = cueTime
				latestTime = math.max(latestTime, cueTime)
			end
			local emitterId = requireField(context, cue, "emitterId", path)
			if checkId(context, emitterId, path .. ".emitterId") and not emitterIds[emitterId] then
				addDiagnostic(context, "UNKNOWN_EMITTER", path .. ".emitterId", "Timeline cue references an unknown particle emitter.")
			end
		end
	end
	return emittedCount, latestTime
end

local function validatePresentation(context: Context, value: any, lifetimeSeconds: number?): (number, number, number, number)
	local path = "$.projectile.presentation"
	local presentation = expectTable(context, value, path)
	if presentation == nil then
		return 0, 0, 0, 0
	end
	checkFields(context, presentation, path, {
		kind = true,
		shape = true,
		size = true,
		color = true,
		transparency = true,
		material = true,
		particles = true,
		curves = true,
		timeline = true,
	}, {
		mesh = "Mesh presentation is deferred; use Presentation.Part in this slice.",
		trail = "Trail presentation is deferred; particle tails are supported in this slice.",
		sound = "Sound presentation is deferred in this slice.",
		light = "Light presentation is deferred in this slice.",
	})
	local kind = requireField(context, presentation, "kind", path)
	if kind ~= nil and kind ~= "Presentation.Part" then
		addDiagnostic(context, "UNSUPPORTED_KIND", path .. ".kind", "Only Presentation.Part is supported in schema version 1.")
	end
	if presentation.shape ~= nil then
		checkEnum(context, presentation.shape, path .. ".shape", { Ball = true, Block = true })
	end
	if presentation.size ~= nil and checkVector(context, presentation.size, path .. ".size") then
		for _, axis in ipairs({ "x", "y", "z" }) do
			local component = presentation.size[axis]
			if component <= 0 or component > Limits.MAX_PART_SIZE then
				addDiagnostic(context, "OUT_OF_BOUNDS", path .. ".size." .. axis, "Part size must be greater than zero and within the part-size cap.")
			end
		end
	end
	if presentation.color ~= nil then
		validateColor(context, presentation.color, path .. ".color")
	end
	if presentation.transparency ~= nil then
		checkNumber(context, presentation.transparency, path .. ".transparency", 0, 1)
	end
	if presentation.material ~= nil then
		checkEnum(context, presentation.material, path .. ".material", {
			Neon = true,
			SmoothPlastic = true,
			ForceField = true,
			Glass = true,
		})
	end

	local particles = presentation.particles or {}
	local curves = presentation.curves or {}
	local timeline = presentation.timeline or {}
	local emitterIds, rateCost, maxParticleLifetime = validateParticles(context, particles, lifetimeSeconds)
	validateCurves(context, curves, lifetimeSeconds)
	local emittedCount, latestTimeline = validateTimeline(context, timeline, emitterIds)
	return rateCost, emittedCount, maxParticleLifetime, latestTimeline
end

local function validateProjectile(context: Context, value: any): number?
	local path = "$.projectile"
	local projectile = expectTable(context, value, path)
	if projectile == nil then
		return nil
	end
	checkFields(context, projectile, path, {
		lifetime = true,
		motion = true,
		presentation = true,
	}, {
		collision = "Collision is a deferred fixed slot in this first compiler slice.",
		tracking = "Tracking is deferred; Bezier uses fixed snapshot control points and does not fake seeking.",
		orientation = "Orientation is deferred in this first compiler slice.",
		ownership = "Ownership is deferred to the server runtime slice.",
		handlers = "Handler graphs are deferred in this first compiler slice.",
	})
	local lifetime = validateLifetime(context, requireField(context, projectile, "lifetime", path))
	validateMotion(context, requireField(context, projectile, "motion", path), lifetime)
	return lifetime
end

local function validateEvents(context: Context, value: any, hasBurst: boolean): number
	local length, events = checkArray(context, value, "$.events", Limits.MAX_EVENTS)
	if length == nil or events == nil then
		return 0
	end
	local ids: { [string]: boolean } = {}
	local burstKinds: { [string]: boolean } = {}
	local priorOrder: number? = nil
	for index = 1, length do
		local path = string.format("$.events[%d]", index)
		local event = expectTable(context, events[index], path)
		if event ~= nil then
			checkFields(context, event, path, { kind = true, id = true, order = true, action = true })
			checkEnum(context, requireField(context, event, "kind", path), path .. ".kind", {
				["Event.Spawn"] = true,
				["Event.Impact"] = true,
				["Event.Expire"] = true,
			})
			local id = requireField(context, event, "id", path)
			if checkId(context, id, path .. ".id") then
				if ids[id] then
					addDiagnostic(context, "DUPLICATE_EVENT_ID", path .. ".id", "Event IDs must be unique.")
				else
					ids[id] = true
				end
			end
			local order = requireField(context, event, "order", path)
			if checkNumber(context, order, path .. ".order", 1, Limits.MAX_EVENTS, true) then
				if priorOrder ~= nil and order <= priorOrder then
					addDiagnostic(context, "INVALID_EVENT_ORDER", path .. ".order", "Event order values must be strictly increasing.")
				end
				priorOrder = order
			end
			local action = event.action or Enums.EventAction.None
			if checkEnum(context, action, path .. ".action", {
				[Enums.EventAction.None] = true,
				[Enums.EventAction.Terminate] = true,
				[Enums.EventAction.Burst] = true,
			})
				and action == Enums.EventAction.Burst
				and not hasBurst
			then
				addDiagnostic(context, "MISSING_BURST", path .. ".action", "Burst action requires a top-level burst definition.")
			end
			if action == Enums.EventAction.Burst and type(event.kind) == "string" then
				if burstKinds[event.kind] then
					addDiagnostic(context, "DUPLICATE_BURST_EVENT", path .. ".action", "Each trigger kind may contain at most one Burst action.")
				else
					burstKinds[event.kind] = true
				end
			end
		end
	end
	return length
end

local function validateRandom(context: Context, value: any, path: string)
	local random = expectTable(context, value, path)
	if random == nil then
		return
	end
	checkFields(context, random, path, { kind = true, seedSource = true, purpose = true }, {
		seed = "Authored seeds are unsafe; the server supplies the root seed and the runtime derives child streams.",
		algorithm = "The shared random algorithm is fixed by RandomStreams format version, not authored data.",
	})
	if random.seed ~= nil or random.rootSeed ~= nil or random.clientSeed ~= nil or random.algorithm ~= nil then
		addDiagnostic(
			context,
			"UNSAFE_RANDOM_CONFIG",
			path,
			"Authored seed or algorithm overrides are forbidden; use the shared server-derived stream."
		)
	end
	local kind = requireField(context, random, "kind", path)
	if kind ~= nil and kind ~= "Random.Derived" then
		addDiagnostic(context, "UNSAFE_RANDOM_CONFIG", path .. ".kind", "Only Random.Derived streams are safe.")
	end
	local seedSource = requireField(context, random, "seedSource", path)
	if seedSource ~= nil and seedSource ~= "Server" then
		addDiagnostic(context, "UNSAFE_RANDOM_CONFIG", path .. ".seedSource", "Random root seeds must be server-owned.")
	end
	local purpose = requireField(context, random, "purpose", path)
	if checkString(context, purpose, path .. ".purpose") then
		if #purpose > Limits.MAX_ID_LENGTH or string.match(purpose, "^[A-Za-z][A-Za-z0-9_.%-]*$") == nil then
			addDiagnostic(context, "UNSAFE_RANDOM_CONFIG", path .. ".purpose", "Random purpose must be a bounded stable label.")
		end
	end
end

local function validateBurst(context: Context, value: any): (number, number)
	local path = "$.burst"
	local burst = expectTable(context, value, path)
	if burst == nil then
		return 0, 0
	end
	checkFields(context, burst, path, {
		kind = true,
		trigger = true,
		distribution = true,
		coneAngleDegrees = true,
		arcMinDegrees = true,
		arcMaxDegrees = true,
		children = true,
		maxDepth = true,
		random = true,
	})
	local kind = requireField(context, burst, "kind", path)
	if kind ~= nil and kind ~= "Burst.Deterministic" then
		addDiagnostic(context, "UNSUPPORTED_KIND", path .. ".kind", "Only Burst.Deterministic is supported.")
	end
	checkEnum(context, requireField(context, burst, "trigger", path), path .. ".trigger", {
		[Enums.BurstTrigger.Impact] = true,
		[Enums.BurstTrigger.Expire] = true,
		[Enums.BurstTrigger.ImpactOrExpire] = true,
	})
	local distribution = requireField(context, burst, "distribution", path)
	checkEnum(context, distribution, path .. ".distribution", {
		[Enums.BurstDistribution.Sphere] = true,
		[Enums.BurstDistribution.Cone] = true,
		[Enums.BurstDistribution.Ring] = true,
		[Enums.BurstDistribution.Arc] = true,
	})
	if distribution == Enums.BurstDistribution.Cone then
		if burst.coneAngleDegrees == nil then
			addDiagnostic(context, "MISSING_FIELD", path .. ".coneAngleDegrees", "Cone bursts require coneAngleDegrees.")
		else
			checkNumber(context, burst.coneAngleDegrees, path .. ".coneAngleDegrees", 0.001, 180)
		end
	elseif burst.coneAngleDegrees ~= nil then
		addDiagnostic(context, "FIELD_NOT_ALLOWED", path .. ".coneAngleDegrees", "Only Cone bursts use coneAngleDegrees.")
	end

	if distribution == Enums.BurstDistribution.Arc then
		local minimum = burst.arcMinDegrees
		local maximum = burst.arcMaxDegrees
		if minimum == nil then
			addDiagnostic(context, "MISSING_FIELD", path .. ".arcMinDegrees", "Arc bursts require arcMinDegrees.")
		else
			checkNumber(context, minimum, path .. ".arcMinDegrees", 0, 179.999)
		end
		if maximum == nil then
			addDiagnostic(context, "MISSING_FIELD", path .. ".arcMaxDegrees", "Arc bursts require arcMaxDegrees.")
		else
			checkNumber(context, maximum, path .. ".arcMaxDegrees", 0.001, 180)
		end
		-- An inverted band is silently empty otherwise: the solver would draw
		-- from a range whose ends cross, and every child would leave along the
		-- same edge. That reads as "the chain does not turn" and is a data bug,
		-- so it is refused here rather than diagnosed by eye.
		if type(minimum) == "number" and type(maximum) == "number" and minimum >= maximum then
			addDiagnostic(
				context,
				"INVALID_ARC_RANGE",
				path .. ".arcMinDegrees",
				"arcMinDegrees must be less than arcMaxDegrees."
			)
		end
	else
		if burst.arcMinDegrees ~= nil then
			addDiagnostic(context, "FIELD_NOT_ALLOWED", path .. ".arcMinDegrees", "Only Arc bursts use arcMinDegrees.")
		end
		if burst.arcMaxDegrees ~= nil then
			addDiagnostic(context, "FIELD_NOT_ALLOWED", path .. ".arcMaxDegrees", "Only Arc bursts use arcMaxDegrees.")
		end
	end

	local totalChildren = 0
	local length, children = checkArray(context, requireField(context, burst, "children", path), path .. ".children", Limits.MAX_BURST_GROUPS)
	local childIds: { [string]: boolean } = {}
	if length ~= nil and children ~= nil then
		if length == 0 then
			addDiagnostic(context, "EMPTY_BURST", path .. ".children", "A burst requires at least one child group.")
		end
		for index = 1, length do
			local childPathValue = string.format("%s.children[%d]", path, index)
			local child = expectTable(context, children[index], childPathValue)
			if child ~= nil then
				checkFields(context, child, childPathValue, { id = true, recipeId = true, count = true })
				local id = requireField(context, child, "id", childPathValue)
				if checkId(context, id, childPathValue .. ".id") then
					if childIds[id] then
						addDiagnostic(context, "DUPLICATE_BURST_GROUP_ID", childPathValue .. ".id", "Burst child group IDs must be unique.")
					else
						childIds[id] = true
					end
				end
				checkId(context, requireField(context, child, "recipeId", childPathValue), childPathValue .. ".recipeId")
				local count = requireField(context, child, "count", childPathValue)
				if checkNumber(context, count, childPathValue .. ".count", 1, Limits.MAX_BURST_CHILDREN, true) then
					totalChildren += count
				end
			end
		end
	end
	if totalChildren > Limits.MAX_BURST_CHILDREN then
		addDiagnostic(context, "BURST_CAP_EXCEEDED", path .. ".children", "Total direct burst children exceed the cap.")
	end
	local maxDepth = requireField(context, burst, "maxDepth", path)
	if not checkNumber(context, maxDepth, path .. ".maxDepth", 1, Limits.MAX_BURST_DEPTH, true) then
		maxDepth = 0
	end
	validateRandom(context, requireField(context, burst, "random", path), path .. ".random")
	return totalChildren, maxDepth
end

type EffectCost = {
	commands: number,
	areaTargets: number,
	statusInstances: number,
	statusTicks: number,
}

local function saturatingAdd(a: number, b: number): number
	if a >= Limits.COST_SATURATION - b then
		return Limits.COST_SATURATION
	end
	return a + b
end

local function saturatingMultiply(a: number, b: number): number
	if a == 0 or b == 0 then
		return 0
	end
	if a >= Limits.COST_SATURATION / b then
		return Limits.COST_SATURATION
	end
	return a * b
end

local function statusTicksForDuration(metadata: any, duration: number): number
	if type(metadata) ~= "table" or metadata.tickInterval == nil or metadata.periodicCount == 0 then
		return 0
	end
	local interval = metadata.tickInterval
	local fullOrdinals = math.floor(duration / interval)
	local exactExpiry = math.abs(fullOrdinals * interval - duration) <= interval * 1e-9
	local tickCount = fullOrdinals + (if metadata.tickOnApply then 1 else 0)
	if exactExpiry and not metadata.tickAtExpiry then
		tickCount = math.max(0, tickCount - 1)
	end
	return tickCount
end

local function validateDamageEffect(context: Context, effect: { [any]: any }, path: string)
	checkFields(context, effect, path, {
		kind = true,
		amount = true,
		damageType = true,
		sourceTags = true,
	})
	checkNumber(
		context,
		requireField(context, effect, "amount", path),
		path .. ".amount",
		0,
		Limits.MAX_EFFECT_AMOUNT
	)
	checkId(context, requireField(context, effect, "damageType", path), path .. ".damageType")
	checkTagArray(context, requireField(context, effect, "sourceTags", path), path .. ".sourceTags")
end

local function validateHealEffect(context: Context, effect: { [any]: any }, path: string)
	checkFields(context, effect, path, { kind = true, amount = true })
	checkNumber(
		context,
		requireField(context, effect, "amount", path),
		path .. ".amount",
		0,
		Limits.MAX_EFFECT_AMOUNT
	)
end

local function validateTagFilters(context: Context, value: { [any]: any }, path: string)
	checkTagArray(context, requireField(context, value, "includeAnyTags", path), path .. ".includeAnyTags")
	checkTagArray(context, requireField(context, value, "includeAllTags", path), path .. ".includeAllTags")
	checkTagArray(context, requireField(context, value, "excludeAnyTags", path), path .. ".excludeAnyTags")
end

local validateEffect: (
	Context,
	any,
	string,
	{ [string]: any },
	{ [string]: number },
	{ [string]: number },
	boolean,
	boolean
) -> EffectCost

validateEffect = function(
	context: Context,
	value: any,
	path: string,
	statusDefinitions: { [string]: any },
	statusTickCosts: { [string]: number },
	statusCommandCosts: { [string]: number },
	allowArea: boolean,
	periodic: boolean
): EffectCost
	local emptyCost: EffectCost = { commands = 0, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
	local effect = expectTable(context, value, path)
	if effect == nil then
		return emptyCost
	end
	local kind = requireField(context, effect, "kind", path)
	if kind == Enums.EffectKind.Damage then
		validateDamageEffect(context, effect, path)
		return { commands = 1, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
	elseif kind == Enums.EffectKind.Heal then
		validateHealEffect(context, effect, path)
		return { commands = 1, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
	elseif periodic then
		checkFields(context, effect, path, { kind = true })
		addDiagnostic(
			context,
			"PERIODIC_EFFECT_NOT_ALLOWED",
			path .. ".kind",
			"Periodic status effects are limited to Effect.Damage and Effect.Heal."
		)
		return emptyCost
	elseif kind == Enums.EffectKind.Area then
		checkFields(context, effect, path, {
			kind = true,
			center = true,
			radius = true,
			falloff = true,
			relationToSource = true,
			includeAnyTags = true,
			includeAllTags = true,
			excludeAnyTags = true,
			maxTargets = true,
			effects = true,
		})
		if not allowArea then
			addDiagnostic(context, "NESTED_AREA", path .. ".kind", "Effect.Area children may not contain another area.")
		end
		checkEnum(context, requireField(context, effect, "center", path), path .. ".center", {
			[Enums.AreaCenter.Impact] = true,
			[Enums.AreaCenter.Projectile] = true,
			[Enums.AreaCenter.Source] = true,
		})
		checkNumber(context, requireField(context, effect, "radius", path), path .. ".radius", 0.001, Limits.MAX_AREA_RADIUS)
		checkEnum(context, requireField(context, effect, "falloff", path), path .. ".falloff", {
			[Enums.AreaFalloff.Constant] = true,
			[Enums.AreaFalloff.Linear] = true,
		})
		checkEnum(
			context,
			requireField(context, effect, "relationToSource", path),
			path .. ".relationToSource",
			{
				[Enums.Relation.Any] = true,
				[Enums.Relation.Self] = true,
				[Enums.Relation.Ally] = true,
				[Enums.Relation.Enemy] = true,
			}
		)
		validateTagFilters(context, effect, path)
		local maxTargets = requireField(context, effect, "maxTargets", path)
		local maxTargetsValid = checkNumber(
			context,
			maxTargets,
			path .. ".maxTargets",
			1,
			Limits.MAX_AREA_TARGETS,
			true
		)
		local childCount, effects = checkArray(
			context,
			requireField(context, effect, "effects", path),
			path .. ".effects",
			Limits.MAX_EFFECTS_PER_AREA
		)
		if childCount == 0 then
			addDiagnostic(context, "EMPTY_AREA_EFFECT", path .. ".effects", "An area requires at least one child effect.")
		end
		local childCost: EffectCost = { commands = 0, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
		if childCount ~= nil and effects ~= nil then
			for index = 1, childCount do
				local cost = validateEffect(
					context,
					effects[index],
					string.format("%s.effects[%d]", path, index),
					statusDefinitions,
					statusTickCosts,
					statusCommandCosts,
					false,
					false
				)
				childCost.commands = saturatingAdd(childCost.commands, cost.commands)
				childCost.areaTargets = saturatingAdd(childCost.areaTargets, cost.areaTargets)
				childCost.statusInstances = saturatingAdd(childCost.statusInstances, cost.statusInstances)
				childCost.statusTicks = saturatingAdd(childCost.statusTicks, cost.statusTicks)
			end
		end
		local targetCount = if maxTargetsValid then maxTargets else 0
		return {
			commands = saturatingMultiply(targetCount, childCost.commands),
			areaTargets = saturatingAdd(targetCount, saturatingMultiply(targetCount, childCost.areaTargets)),
			statusInstances = saturatingMultiply(targetCount, childCost.statusInstances),
			statusTicks = saturatingMultiply(targetCount, childCost.statusTicks),
		}
	elseif kind == Enums.EffectKind.Pull then
		checkFields(context, effect, path, { kind = true, center = true, deltaSpeed = true, maxDeltaSpeed = true })
		checkEnum(context, requireField(context, effect, "center", path), path .. ".center", {
			[Enums.AreaCenter.Impact] = true,
			[Enums.AreaCenter.Projectile] = true,
			[Enums.AreaCenter.Source] = true,
		})
		checkNumber(
			context,
			requireField(context, effect, "deltaSpeed", path),
			path .. ".deltaSpeed",
			0,
			Limits.MAX_PULL_SPEED
		)
		checkNumber(
			context,
			requireField(context, effect, "maxDeltaSpeed", path),
			path .. ".maxDeltaSpeed",
			0,
			Limits.MAX_PULL_SPEED
		)
		return { commands = 1, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
	elseif kind == Enums.EffectKind.ApplyStatus then
		checkFields(context, effect, path, {
			kind = true,
			definitionId = true,
			definitionVersion = true,
			stacks = true,
			potency = true,
			durationOverride = true,
		})
		local definitionId = requireField(context, effect, "definitionId", path)
		local definitionVersion = requireField(context, effect, "definitionVersion", path)
		local idOk = checkId(context, definitionId, path .. ".definitionId")
		local versionOk = checkNumber(
			context,
			definitionVersion,
			path .. ".definitionVersion",
			1,
			2147483647,
			true
		)
		if idOk and versionOk and not statusDefinitions[definitionId .. "@" .. tostring(definitionVersion)] then
			addDiagnostic(
				context,
				"MISSING_STATUS_DEFINITION",
				path .. ".definitionVersion",
				"Effect.ApplyStatus references a missing status definition ID/version."
			)
		end
		checkNumber(context, requireField(context, effect, "stacks", path), path .. ".stacks", 1, Limits.MAX_STATUS_STACKS, true)
		checkNumber(
			context,
			requireField(context, effect, "potency", path),
			path .. ".potency",
			0,
			Limits.MAX_EFFECT_AMOUNT
		)
		if effect.durationOverride ~= nil then
			local durationOverrideOk = checkNumber(
				context,
				effect.durationOverride,
				path .. ".durationOverride",
				0.001,
				Limits.MAX_STATUS_DURATION
			)
			local key = if type(definitionId) == "string" and type(definitionVersion) == "number"
				then definitionId .. "@" .. tostring(definitionVersion)
				else ""
			local overrideTicks = if durationOverrideOk
				then statusTicksForDuration(statusDefinitions[key], effect.durationOverride)
				else 0
			if overrideTicks > Limits.MAX_STATUS_TICKS_PER_INSTANCE then
				addDiagnostic(
					context,
					"STATUS_TICK_CAP_EXCEEDED",
					path .. ".durationOverride",
					"Duration override exceeds the per-instance status tick cap."
				)
			end
		end
		local key = if type(definitionId) == "string" and type(definitionVersion) == "number"
			then definitionId .. "@" .. tostring(definitionVersion)
			else ""
		local metadata = statusDefinitions[key]
		local tickCost = if effect.durationOverride ~= nil and type(effect.durationOverride) == "number"
			then statusTicksForDuration(metadata, effect.durationOverride)
			else statusTickCosts[key] or 0
		local commandCost = if type(metadata) ~= "table"
			then statusCommandCosts[key] or 0
			else saturatingMultiply(tickCost, metadata.periodicCount)
		return {
			commands = saturatingAdd(1, commandCost),
			areaTargets = 0,
			statusInstances = 1,
			statusTicks = tickCost,
		}
	elseif kind == Enums.EffectKind.Cleanse then
		checkFields(context, effect, path, {
			kind = true,
			includeAnyTags = true,
			includeAllTags = true,
			excludeAnyTags = true,
			maxRemovals = true,
		})
		validateTagFilters(context, effect, path)
		checkNumber(
			context,
			requireField(context, effect, "maxRemovals", path),
			path .. ".maxRemovals",
			1,
			Limits.MAX_STATUS_BUDGET,
			true
		)
		return { commands = 1, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
	else
		checkFields(context, effect, path, { kind = true })
		if kind ~= nil then
			addDiagnostic(context, "UNSUPPORTED_KIND", path .. ".kind", "Unsupported gameplay effect kind.")
		end
		return emptyCost
	end
end

local function validateStatusDefinitions(
	context: Context,
	value: any
): ({ [string]: any }, { [string]: number }, { [string]: number }, number)
	local registry: { [string]: any } = {}
	local tickCosts: { [string]: number } = {}
	local commandCosts: { [string]: number } = {}
	local maximumTicks = 0
	local length, definitions = checkArray(context, value, "$.statusDefinitions", Limits.MAX_STATUS_DEFINITIONS)
	if length == nil or definitions == nil then
		return registry, tickCosts, commandCosts, maximumTicks
	end
	for index = 1, length do
		local path = string.format("$.statusDefinitions[%d]", index)
		local definition = expectTable(context, definitions[index], path)
		if definition ~= nil then
			checkFields(context, definition, path, {
				id = true,
				version = true,
				tags = true,
				duration = true,
				tickInterval = true,
				tickOnApply = true,
				tickAtExpiry = true,
				stackPolicy = true,
				maxStacks = true,
				maxIndependentInstances = true,
				periodicEffects = true,
				modifiers = true,
				cueId = true,
			})
			local id = requireField(context, definition, "id", path)
			local version = requireField(context, definition, "version", path)
			local idOk = checkId(context, id, path .. ".id")
			local versionOk = checkNumber(context, version, path .. ".version", 1, 2147483647, true)
			if idOk and versionOk then
				local key = id .. "@" .. tostring(version)
				if registry[key] then
					addDiagnostic(
						context,
						"DUPLICATE_STATUS_DEFINITION",
						path .. ".version",
						"Status definition ID/version pairs must be unique."
					)
				else
					registry[key] = true
				end
			end
			checkTagArray(context, requireField(context, definition, "tags", path), path .. ".tags")
			local duration = requireField(context, definition, "duration", path)
			local durationOk = checkNumber(context, duration, path .. ".duration", 0.001, Limits.MAX_STATUS_DURATION)
			local interval = definition.tickInterval
			local intervalOk = false
			if interval ~= nil then
				intervalOk = checkNumber(
					context,
					interval,
					path .. ".tickInterval",
					Limits.MIN_STATUS_TICK_INTERVAL,
					Limits.MAX_STATUS_DURATION
				)
			end
			local tickOnApply = requireField(context, definition, "tickOnApply", path)
			local tickAtExpiry = requireField(context, definition, "tickAtExpiry", path)
			checkBoolean(context, tickOnApply, path .. ".tickOnApply")
			checkBoolean(context, tickAtExpiry, path .. ".tickAtExpiry")
			local stackPolicy = requireField(context, definition, "stackPolicy", path)
			checkEnum(context, stackPolicy, path .. ".stackPolicy", {
				RefreshDuration = true,
				AddStacks = true,
				ReplaceIfStronger = true,
				Independent = true,
			})
			checkNumber(
				context,
				requireField(context, definition, "maxStacks", path),
				path .. ".maxStacks",
				1,
				Limits.MAX_STATUS_STACKS,
				true
			)
			local independentCap = requireField(context, definition, "maxIndependentInstances", path)
			if checkNumber(
				context,
				independentCap,
				path .. ".maxIndependentInstances",
				1,
				Limits.MAX_INDEPENDENT_STATUS_INSTANCES,
				true
			) and stackPolicy ~= "Independent" and independentCap ~= 1 then
				addDiagnostic(
					context,
					"FIELD_NOT_ALLOWED",
					path .. ".maxIndependentInstances",
					"Non-independent statuses must use maxIndependentInstances = 1."
				)
			end
			local periodicCount, periodicEffects = checkArray(
				context,
				requireField(context, definition, "periodicEffects", path),
				path .. ".periodicEffects",
				Limits.MAX_PERIODIC_EFFECTS
			)
			if periodicCount ~= nil and periodicEffects ~= nil then
				if periodicCount > 0 and interval == nil then
					addDiagnostic(
						context,
						"MISSING_FIELD",
						path .. ".tickInterval",
						"Periodic status effects require tickInterval."
					)
				end
				for effectIndex = 1, periodicCount do
					validateEffect(
						context,
						periodicEffects[effectIndex],
						string.format("%s.periodicEffects[%d]", path, effectIndex),
						registry,
						{},
						{},
						false,
						true
					)
				end
			end
			if (tickOnApply == true or tickAtExpiry == true) and interval == nil then
				addDiagnostic(context, "MISSING_FIELD", path .. ".tickInterval", "Tick flags require tickInterval.")
			end
			if durationOk and intervalOk then
				local metadata = {
					tickInterval = interval,
					tickOnApply = tickOnApply == true,
					tickAtExpiry = tickAtExpiry == true,
					periodicCount = periodicCount or 0,
				}
				local tickCount = statusTicksForDuration(metadata, duration)
				maximumTicks = math.max(maximumTicks, tickCount)
				if idOk and versionOk then
					local key = id .. "@" .. tostring(version)
					registry[key] = metadata
					tickCosts[key] = tickCount
					commandCosts[key] = saturatingMultiply(tickCount, periodicCount or 0)
				end
				if tickCount > Limits.MAX_STATUS_TICKS_PER_INSTANCE then
					addDiagnostic(
						context,
						"STATUS_TICK_CAP_EXCEEDED",
						path .. ".tickInterval",
						"Status duration and interval exceed the per-instance tick cap."
					)
				end
			end
			local modifierCount, modifiers = checkArray(
				context,
				requireField(context, definition, "modifiers", path),
				path .. ".modifiers",
				Limits.MAX_STATUS_MODIFIERS
			)
			local modifierIds: { [string]: boolean } = {}
			if modifierCount ~= nil and modifiers ~= nil then
				for modifierIndex = 1, modifierCount do
					local modifierPath = string.format("%s.modifiers[%d]", path, modifierIndex)
					local modifier = expectTable(context, modifiers[modifierIndex], modifierPath)
					if modifier ~= nil then
						checkFields(context, modifier, modifierPath, {
							id = true,
							stat = true,
							operation = true,
							value = true,
						})
						local modifierId = requireField(context, modifier, "id", modifierPath)
						if checkId(context, modifierId, modifierPath .. ".id") then
							if modifierIds[modifierId] then
								addDiagnostic(context, "DUPLICATE_MODIFIER_ID", modifierPath .. ".id", "Modifier IDs must be unique in one status.")
							else
								modifierIds[modifierId] = true
							end
						end
						checkId(context, requireField(context, modifier, "stat", modifierPath), modifierPath .. ".stat")
						checkEnum(
							context,
							requireField(context, modifier, "operation", modifierPath),
							modifierPath .. ".operation",
							{ Add = true, Multiply = true }
						)
						checkNumber(
							context,
							requireField(context, modifier, "value", modifierPath),
							modifierPath .. ".value",
							-Limits.MAX_EFFECT_AMOUNT,
							Limits.MAX_EFFECT_AMOUNT
						)
					end
				end
			end
			if definition.cueId ~= nil then
				checkId(context, definition.cueId, path .. ".cueId")
			end
		end
	end
	return registry, tickCosts, commandCosts, maximumTicks
end

local function validateEffectTriggers(
	context: Context,
	value: any,
	statusDefinitions: { [string]: any },
	statusTickCosts: { [string]: number },
	statusCommandCosts: { [string]: number }
): EffectCost
	local total: EffectCost = { commands = 0, areaTargets = 0, statusInstances = 0, statusTicks = 0 }
	local length, triggers = checkArray(context, value, "$.effectTriggers", Limits.MAX_EFFECT_TRIGGERS)
	if length == nil or triggers == nil then
		return total
	end
	local ids: { [string]: boolean } = {}
	local priorOrder: number? = nil
	for index = 1, length do
		local path = string.format("$.effectTriggers[%d]", index)
		local trigger = expectTable(context, triggers[index], path)
		if trigger ~= nil then
			checkFields(context, trigger, path, { id = true, trigger = true, order = true, effect = true, cueIds = true })
			local id = requireField(context, trigger, "id", path)
			if checkId(context, id, path .. ".id") then
				if ids[id] then
					addDiagnostic(context, "DUPLICATE_EFFECT_TRIGGER_ID", path .. ".id", "Effect trigger IDs must be unique.")
				else
					ids[id] = true
				end
			end
			local triggerName = requireField(context, trigger, "trigger", path)
			if type(triggerName) == "string"
				and triggerName ~= Enums.Trigger.Impact
				and triggerName ~= Enums.Trigger.Expire
			then
				addDiagnostic(context, "UNSUPPORTED_TRIGGER", path .. ".trigger", "Only Impact and Expire effect triggers are supported.")
			elseif type(triggerName) ~= "string" then
				addDiagnostic(context, "TYPE_MISMATCH", path .. ".trigger", "Expected a string enum value.")
			end
			local order = requireField(context, trigger, "order", path)
			if checkNumber(context, order, path .. ".order", 1, Limits.MAX_EFFECT_TRIGGERS, true) then
				if priorOrder ~= nil and order <= priorOrder then
					addDiagnostic(context, "INVALID_EFFECT_TRIGGER_ORDER", path .. ".order", "Effect trigger order values must be strictly increasing.")
				end
				priorOrder = order
			end
			local authoredCueIds = if trigger.cueIds == nil then {} else trigger.cueIds
			local cueCount, cueIds = checkArray(
				context,
				authoredCueIds,
				path .. ".cueIds",
				Limits.MAX_EFFECT_CUES_PER_TRIGGER
			)
			local seenCueIds: { [string]: boolean } = {}
			if cueCount ~= nil and cueIds ~= nil then
				for cueIndex = 1, cueCount do
					local cuePath = string.format("%s.cueIds[%d]", path, cueIndex)
					local cueId = cueIds[cueIndex]
					if checkId(context, cueId, cuePath) then
						if seenCueIds[cueId] then
							addDiagnostic(context, "DUPLICATE_EFFECT_CUE_ID", cuePath, "Effect trigger cue IDs must be unique.")
						else
							seenCueIds[cueId] = true
						end
					end
				end
			end
			local cost = validateEffect(
				context,
				requireField(context, trigger, "effect", path),
				path .. ".effect",
				statusDefinitions,
				statusTickCosts,
				statusCommandCosts,
				true,
				false
			)
			total.commands = saturatingAdd(total.commands, cost.commands)
			total.areaTargets = saturatingAdd(total.areaTargets, cost.areaTargets)
			total.statusInstances = saturatingAdd(total.statusInstances, cost.statusInstances)
			total.statusTicks = saturatingAdd(total.statusTicks, cost.statusTicks)
		end
	end
	return total
end

local function defaultBudgets(): { [string]: number }
	return {
		maxProjectiles = Types.DefaultBudgets.maxProjectiles,
		maxEvents = Types.DefaultBudgets.maxEvents,
		maxParticles = Types.DefaultBudgets.maxParticles,
		maxBurstDepth = Types.DefaultBudgets.maxBurstDepth,
		maxEffectCommands = Types.DefaultBudgets.maxEffectCommands,
		maxAreaTargets = Types.DefaultBudgets.maxAreaTargets,
		maxStatusesPerActor = Types.DefaultBudgets.maxStatusesPerActor,
		maxStatusTicksPerAdvance = Types.DefaultBudgets.maxStatusTicksPerAdvance,
	}
end

local function validateBudgets(context: Context, value: any): { [string]: number }
	if value == nil then
		return defaultBudgets()
	end
	local path = "$.budgets"
	local budgets = expectTable(context, value, path)
	if budgets == nil then
		return defaultBudgets()
	end
	checkFields(context, budgets, path, {
		maxProjectiles = true,
		maxEvents = true,
		maxParticles = true,
		maxBurstDepth = true,
		maxEffectCommands = true,
		maxAreaTargets = true,
		maxStatusesPerActor = true,
		maxStatusTicksPerAdvance = true,
	})
	local result = defaultBudgets()
	local specs = {
		{ field = "maxProjectiles", maximum = Limits.MAX_PROJECTILE_BUDGET, required = true },
		{ field = "maxEvents", maximum = Limits.MAX_EVENT_BUDGET, required = true },
		{ field = "maxParticles", maximum = Limits.MAX_PARTICLE_BUDGET, required = true },
		{ field = "maxBurstDepth", maximum = Limits.MAX_BURST_DEPTH, required = true },
		{ field = "maxEffectCommands", maximum = Limits.MAX_EFFECT_COMMAND_BUDGET, required = false },
		{ field = "maxAreaTargets", maximum = Limits.MAX_AREA_TARGETS, required = false },
		{ field = "maxStatusesPerActor", maximum = Limits.MAX_STATUS_BUDGET, required = false },
		{
			field = "maxStatusTicksPerAdvance",
			maximum = Limits.MAX_STATUS_TICKS_PER_ADVANCE_BUDGET,
			required = false,
		},
	}
	for _, spec in ipairs(specs) do
		local field = spec.field
		local maximum = spec.maximum
		local fieldValue = budgets[field]
		if fieldValue == nil and spec.required then
			fieldValue = requireField(context, budgets, field, path)
		end
		if fieldValue ~= nil and checkNumber(context, fieldValue, path .. "." .. field, 1, maximum, true) then
			result[field] = fieldValue
		end
	end
	return result
end

local function worstCaseProjectiles(childCount: number, depth: number, stopAbove: number): number
	local total = 1
	local layer = 1
	for _ = 1, depth do
		layer = saturatingMultiply(layer, childCount)
		total = saturatingAdd(total, layer)
		if total > stopAbove then
			return total
		end
	end
	return total
end

function Validator.validate(document: any, options: any?): (boolean, { Diagnostic })
	local context: Context = {
		diagnostics = {},
		nonfinitePaths = {},
	}
	if not scanPureData(context, document) then
		return false, context.diagnostics
	end

	local root = expectTable(context, document, "$")
	if root == nil then
		return false, context.diagnostics
	end
	checkFields(context, root, "$", {
		schemaVersion = true,
		id = true,
		revision = true,
		projectile = true,
		burst = true,
		events = true,
		effectTriggers = true,
		statusDefinitions = true,
		budgets = true,
	}, {
		metadata = "Metadata is deferred until canonical serialization is locked.",
		tracking = "Tracking is a projectile slot and is deferred in this slice.",
	})

	local schemaVersion = requireField(context, root, "schemaVersion", "$")
	if checkNumber(context, schemaVersion, "$.schemaVersion", nil, nil, true) and schemaVersion ~= 1 then
		addDiagnostic(context, "UNSUPPORTED_SCHEMA_VERSION", "$.schemaVersion", "Only schemaVersion 1 is supported.")
	end
	checkId(context, requireField(context, root, "id", "$"), "$.id")
	checkNumber(context, requireField(context, root, "revision", "$"), "$.revision", 1, 2147483647, true)

	local lifetimeSeconds = validateProjectile(context, requireField(context, root, "projectile", "$"))
	local rateCost = 0
	local timelineEmitCost = 0
	local emitterCount = 0
	local timelineCount = 0
	if type(root.projectile) == "table" and type(root.projectile.presentation) == "table" then
		local _, _, _, _latest
		rateCost, timelineEmitCost, _, _latest = validatePresentation(context, root.projectile.presentation, lifetimeSeconds)
		local particles = root.projectile.presentation.particles or {}
		local timeline = root.projectile.presentation.timeline or {}
		emitterCount = TableUtil.denseArrayLength(particles) or 0
		timelineCount = TableUtil.denseArrayLength(timeline) or 0
	end

	local hasBurst = root.burst ~= nil
	validateEvents(context, root.events or {}, hasBurst)
	local childCount = 0
	local burstDepth = 0
	if hasBurst then
		childCount, burstDepth = validateBurst(context, root.burst)
	end
	local statusDefinitions, statusTickCosts, statusCommandCosts, _maximumStatusTicks = validateStatusDefinitions(
		context,
		root.statusDefinitions or {}
	)
	local effectCost = validateEffectTriggers(
		context,
		root.effectTriggers or {},
		statusDefinitions,
		statusTickCosts,
		statusCommandCosts
	)
	local budgets = validateBudgets(context, root.budgets)

	if burstDepth > budgets.maxBurstDepth then
		addDiagnostic(context, "BURST_CAP_EXCEEDED", "$.burst.maxDepth", "Burst depth exceeds the document budget.")
	end
	if options == nil or options.skipRecursiveProjectileBudget ~= true then
		local projectileCost = worstCaseProjectiles(childCount, burstDepth, budgets.maxProjectiles)
		if projectileCost > budgets.maxProjectiles then
			addDiagnostic(context, "BURST_CAP_EXCEEDED", "$.burst.children", "Worst-case projectile fan-out exceeds maxProjectiles.")
		end
	end
	local eventCost = saturatingAdd(3, emitterCount)
	eventCost = saturatingAdd(eventCost, timelineCount)
	eventCost = saturatingAdd(eventCost, childCount)
	if eventCost > budgets.maxEvents then
		addDiagnostic(context, "EVENT_CAP_EXCEEDED", "$.budgets.maxEvents", "Worst-case scheduled projectile work exceeds maxEvents.")
	end
	if saturatingAdd(rateCost, timelineEmitCost) > budgets.maxParticles then
		addDiagnostic(context, "PARTICLE_CAP_EXCEEDED", "$.budgets.maxParticles", "Worst-case particle emissions exceed maxParticles.")
	end
	if effectCost.commands > budgets.maxEffectCommands then
		addDiagnostic(context, "EFFECT_COMMAND_CAP_EXCEEDED", "$.budgets.maxEffectCommands", "Worst-case effect commands exceed maxEffectCommands.")
	end
	if effectCost.areaTargets > budgets.maxAreaTargets then
		addDiagnostic(context, "AREA_TARGET_CAP_EXCEEDED", "$.budgets.maxAreaTargets", "Worst-case area targets exceed maxAreaTargets.")
	end
	if effectCost.statusInstances > budgets.maxStatusesPerActor then
		addDiagnostic(context, "STATUS_CAP_EXCEEDED", "$.budgets.maxStatusesPerActor", "Worst-case status applications to one target exceed maxStatusesPerActor.")
	end

	local valid = true
	for _, diagnostic in ipairs(context.diagnostics) do
		if diagnostic.severity == "Error" then
			valid = false
			break
		end
	end
	return valid, context.diagnostics
end

function Validator.diagnose(document: any): { Diagnostic }
	local _, diagnostics = Validator.validate(document)
	return diagnostics
end

function Validator.isValid(document: any): boolean
	local valid = Validator.validate(document)
	return valid
end

Validator.Validate = Validator.validate
Validator.Diagnose = Validator.diagnose
Validator.IsValid = Validator.isValid

return table.freeze(Validator)
