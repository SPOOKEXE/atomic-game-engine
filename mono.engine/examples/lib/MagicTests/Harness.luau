--!strict

export type Report = {
	label: string,
	passed: number,
	failed: number,
	total: number,
	declared: number,
	executed: number,
	failures: { string },
	summary: string,
}

export type Harness = {
	_label: string,
	_suiteName: string,
	_passed: number,
	_failed: number,
	_declared: number,
	_executed: number,
	_failures: { string },

	suite: (self: Harness, name: string, define: (Harness) -> ()) -> (),
	test: (self: Harness, name: string, run: () -> ()) -> (),
	assertEqual: (self: Harness, actual: any, expected: any, message: string?) -> (),
	assertDeepEqual: (self: Harness, actual: any, expected: any, message: string?) -> (),
	assertNear: (self: Harness, actual: number, expected: number, epsilon: number?, message: string?) -> (),
	assertTrue: (self: Harness, value: any, message: string?) -> (),
	assertNil: (self: Harness, value: any, message: string?) -> (),
	assertThrows: (self: Harness, run: () -> (), contains: string?) -> (),
	assertDiagnostic: (self: Harness, diagnostics: { any }, code: string, path: string) -> any,
	report: (self: Harness) -> Report,
}

local Harness = {}
Harness.__index = Harness

local function fail(message: string): never
	error(message, 3)
end

local function formatValue(value: any): string
	if type(value) == "string" then
		return string.format("%q", value)
	end
	return tostring(value)
end

local function isFiniteNumber(value: any): boolean
	return type(value) == "number" and value == value and math.abs(value) ~= math.huge
end

local function compareDeep(actual: any, expected: any, path: string, seen: { [any]: any }): string?
	if type(actual) ~= type(expected) then
		return string.format("%s type mismatch: %s ~= %s", path, type(actual), type(expected))
	end
	if type(actual) ~= "table" then
		if actual ~= expected then
			return string.format("%s mismatch: %s ~= %s", path, formatValue(actual), formatValue(expected))
		end
		return nil
	end

	local prior = seen[actual]
	if prior ~= nil then
		if prior ~= expected then
			return path .. " table sharing differs"
		end
		return nil
	end
	seen[actual] = expected

	for key, expectedChild in pairs(expected) do
		if actual[key] == nil and expectedChild ~= nil then
			return string.format("%s.%s is missing", path, tostring(key))
		end
		local difference = compareDeep(actual[key], expectedChild, string.format("%s.%s", path, tostring(key)), seen)
		if difference ~= nil then
			return difference
		end
	end
	for key in pairs(actual) do
		if expected[key] == nil and actual[key] ~= nil then
			return string.format("%s.%s is unexpected", path, tostring(key))
		end
	end
	return nil
end

local function traceError(message: any): string
	return debug.traceback(tostring(message), 2)
end

function Harness.new(label: string): Harness
	local self = setmetatable({
		_label = label,
		_suiteName = "root",
		_passed = 0,
		_failed = 0,
		_declared = 0,
		_executed = 0,
		_failures = {},
	}, Harness)
	return (self :: unknown) :: Harness
end

function Harness.suite(self: Harness, name: string, define: (Harness) -> ())
	local priorSuite = self._suiteName
	self._suiteName = name
	local outcome = table.pack(xpcall(function()
		define(self)
	end, traceError))
	local ok = outcome[1] == true
	local problem = tostring(outcome[2])
	if not ok then
		self._failed += 1
		table.insert(self._failures, string.format("%s / <suite setup>: %s", name, problem))
	end
	self._suiteName = priorSuite
end

function Harness.test(self: Harness, name: string, run: () -> ())
	self._declared += 1
	local outcome = table.pack(xpcall(run, traceError))
	self._executed += 1
	local ok = outcome[1] == true
	local problem = tostring(outcome[2])
	if ok then
		self._passed += 1
	else
		self._failed += 1
		table.insert(self._failures, string.format("%s / %s: %s", self._suiteName, name, problem))
	end
end

function Harness.assertEqual(_self: Harness, actual: any, expected: any, message: string?)
	if actual ~= expected then
		fail(message or string.format("expected %s, got %s", formatValue(expected), formatValue(actual)))
	end
end

function Harness.assertDeepEqual(_self: Harness, actual: any, expected: any, message: string?)
	local difference = compareDeep(actual, expected, "$", {})
	if difference ~= nil then
		fail((message and (message .. ": ") or "") .. difference)
	end
end

function Harness.assertNear(_self: Harness, actual: number, expected: number, epsilon: number?, message: string?)
	local tolerance = if epsilon == nil then 1e-9 else epsilon
	if not isFiniteNumber(actual) or not isFiniteNumber(expected) then
		fail(message or "assertNear actual and expected values must be finite numbers")
	end
	if not isFiniteNumber(tolerance) or tolerance < 0 then
		fail(message or "assertNear tolerance must be finite and non-negative")
	end
	if math.abs(actual - expected) > tolerance then
		fail(message or string.format("expected %.12g +/- %.12g, got %.12g", expected, tolerance, actual))
	end
end

function Harness.assertTrue(_self: Harness, value: any, message: string?)
	if value ~= true then
		fail(message or "expected true")
	end
end

function Harness.assertNil(_self: Harness, value: any, message: string?)
	if value ~= nil then
		fail(message or string.format("expected nil, got %s", formatValue(value)))
	end
end

function Harness.assertThrows(_self: Harness, run: () -> (), contains: string?)
	local ok, problem = pcall(run)
	if ok then
		fail("expected function to throw")
	end
	if contains ~= nil and string.find(tostring(problem), contains, 1, true) == nil then
		fail(string.format("error did not contain %q: %s", contains, tostring(problem)))
	end
end

function Harness.assertDiagnostic(_self: Harness, diagnostics: { any }, code: string, path: string): any
	for _, diagnostic in ipairs(diagnostics) do
		if diagnostic.code == code and diagnostic.path == path then
			return diagnostic
		end
	end
	return fail(string.format("missing diagnostic %s at %s", code, path))
end

function Harness.report(self: Harness): Report
	return {
		label = self._label,
		passed = self._passed,
		failed = self._failed,
		total = self._executed,
		declared = self._declared,
		executed = self._executed,
		failures = table.clone(self._failures),
		summary = string.format(
			"[MagicTests] %s: %d passed, %d failed, %d executed, %d declared",
			self._label,
			self._passed,
			self._failed,
			self._executed,
			self._declared
		),
	}
end

Harness.New = Harness.new

return table.freeze(Harness)
