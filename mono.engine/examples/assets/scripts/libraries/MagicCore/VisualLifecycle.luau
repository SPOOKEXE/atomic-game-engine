--!strict

export type Lifetime = {
	min: number?,
	max: number,
}

export type EmitterRegistration = {
	id: string,
	lifetime: Lifetime?,
	lifetimeMax: number?,
}

export type EmitterState = {
	id: string,
	lifetimeMax: number,
	disableAt: number?,
	latestEmitAt: number?,
}

export type RequestKind = "Release" | "Cancel"

export type State = {
	requestKind: RequestKind?,
	requestAt: number?,
	releaseAt: number?,
	emitters: { EmitterState },
}

export type Tracker = {
	_emittersById: { [string]: EmitterState },
	_emitterOrder: { string },
	_requestKind: RequestKind?,
	_requestAt: number?,
	_releaseAt: number?,

	RegisterEmitter: (
		self: Tracker,
		registrationOrId: EmitterRegistration | string,
		lifetimeMax: number?
	) -> (EmitterState, boolean),
	RecordEmission: (self: Tracker, emitterId: string, emitAt: number) -> boolean,
	DisableEmitter: (self: Tracker, emitterId: string, disableAt: number) -> boolean,
	DisableRemaining: (self: Tracker, disableAt: number?) -> { string },
	RequestRelease: (self: Tracker, requestAt: number) -> (number, { string }),
	RequestCancel: (self: Tracker, requestAt: number) -> (number, { string }),
	CanRelease: (self: Tracker, now: number) -> boolean,
	GetReleaseAt: (self: Tracker) -> number?,
	GetEmitter: (self: Tracker, emitterId: string) -> EmitterState?,
	GetState: (self: Tracker) -> State,
}

local VisualLifecycle = {}
VisualLifecycle.__index = VisualLifecycle

local function isFinite(value: number): boolean
	return value == value and math.abs(value) ~= math.huge
end

local function copyEmitter(emitter: EmitterState): EmitterState
	return {
		id = emitter.id,
		lifetimeMax = emitter.lifetimeMax,
		disableAt = emitter.disableAt,
		latestEmitAt = emitter.latestEmitAt,
	}
end

local function recalculateReleaseAt(self: Tracker): number?
	local requestAt = self._requestAt
	if requestAt == nil then
		self._releaseAt = nil
		return nil
	end

	local releaseAt = requestAt
	for _, emitterId in ipairs(self._emitterOrder) do
		local emitter = self._emittersById[emitterId]
		local disableAt = emitter.disableAt
		if disableAt == nil then
			self._releaseAt = nil
			return nil
		end

		releaseAt = math.max(releaseAt, disableAt + emitter.lifetimeMax)
		local latestEmitAt = emitter.latestEmitAt
		if latestEmitAt ~= nil then
			releaseAt = math.max(releaseAt, latestEmitAt + emitter.lifetimeMax)
		end
	end

	self._releaseAt = releaseAt
	return releaseAt
end

function VisualLifecycle.new(initialEmitters: { EmitterRegistration }?): Tracker
	local self = setmetatable({
		_emittersById = {},
		_emitterOrder = {},
		_requestKind = nil,
		_requestAt = nil,
		_releaseAt = nil,
	}, VisualLifecycle)

	local tracker = (self :: unknown) :: Tracker
	if initialEmitters ~= nil then
		for _, emitter in ipairs(initialEmitters) do
			tracker:RegisterEmitter(emitter)
		end
	end
	return tracker
end

function VisualLifecycle.RegisterEmitter(
	self: Tracker,
	registrationOrId: EmitterRegistration | string,
	lifetimeMax: number?
): (EmitterState, boolean)
	local emitterId: string
	local resolvedLifetimeMax: number?
	if type(registrationOrId) == "string" then
		emitterId = registrationOrId
		resolvedLifetimeMax = lifetimeMax
	else
		emitterId = registrationOrId.id
		resolvedLifetimeMax = registrationOrId.lifetimeMax
		if resolvedLifetimeMax == nil and registrationOrId.lifetime ~= nil then
			resolvedLifetimeMax = registrationOrId.lifetime.max
		end
	end

	assert(emitterId ~= "", "emitter id must not be empty")
	assert(
		resolvedLifetimeMax ~= nil and isFinite(resolvedLifetimeMax) and resolvedLifetimeMax >= 0,
		"emitter lifetime max must be finite and non-negative"
	)

	local existing = self._emittersById[emitterId]
	if existing ~= nil then
		assert(existing.lifetimeMax == resolvedLifetimeMax, "duplicate emitter id has a different lifetime")
		return copyEmitter(existing), false
	end

	local emitter: EmitterState = {
		id = emitterId,
		lifetimeMax = resolvedLifetimeMax,
		disableAt = self._requestAt,
		latestEmitAt = nil,
	}
	self._emittersById[emitterId] = emitter
	table.insert(self._emitterOrder, emitterId)
	recalculateReleaseAt(self)
	return copyEmitter(emitter), true
end

function VisualLifecycle.RecordEmission(self: Tracker, emitterId: string, emitAt: number): boolean
	assert(isFinite(emitAt), "emitAt must be finite")

	local emitter = self._emittersById[emitterId]
	if emitter == nil then
		return false
	end

	local latestEmitAt = emitter.latestEmitAt
	if latestEmitAt ~= nil and latestEmitAt >= emitAt then
		return false
	end

	emitter.latestEmitAt = emitAt
	recalculateReleaseAt(self)
	return true
end

function VisualLifecycle.DisableEmitter(self: Tracker, emitterId: string, disableAt: number): boolean
	assert(isFinite(disableAt), "disableAt must be finite")

	local emitter = self._emittersById[emitterId]
	if emitter == nil or emitter.disableAt ~= nil then
		return false
	end

	emitter.disableAt = disableAt
	recalculateReleaseAt(self)
	return true
end

function VisualLifecycle.DisableRemaining(self: Tracker, disableAt: number?): { string }
	local resolvedDisableAt = disableAt or self._requestAt
	assert(resolvedDisableAt ~= nil and isFinite(resolvedDisableAt), "disableAt must be finite")

	local disabledIds = {}
	for _, emitterId in ipairs(self._emitterOrder) do
		local emitter = self._emittersById[emitterId]
		if emitter.disableAt == nil then
			emitter.disableAt = resolvedDisableAt
			table.insert(disabledIds, emitterId)
		end
	end
	recalculateReleaseAt(self)
	return disabledIds
end

local function request(self: Tracker, requestKind: RequestKind, requestAt: number): (number, { string })
	assert(isFinite(requestAt), "requestAt must be finite")

	if self._requestAt == nil then
		self._requestAt = requestAt
		self._requestKind = requestKind
	end

	local effectiveRequestAt = self._requestAt :: number
	local disabledIds = self:DisableRemaining(effectiveRequestAt)
	local releaseAt = recalculateReleaseAt(self)
	assert(releaseAt ~= nil, "releaseAt must exist after remaining emitters are disabled")
	return releaseAt, disabledIds
end

function VisualLifecycle.RequestRelease(self: Tracker, requestAt: number): (number, { string })
	return request(self, "Release", requestAt)
end

function VisualLifecycle.RequestCancel(self: Tracker, requestAt: number): (number, { string })
	return request(self, "Cancel", requestAt)
end

function VisualLifecycle.CanRelease(self: Tracker, now: number): boolean
	assert(isFinite(now), "now must be finite")
	local releaseAt = self._releaseAt
	return releaseAt ~= nil and now >= releaseAt
end

function VisualLifecycle.GetReleaseAt(self: Tracker): number?
	return self._releaseAt
end

function VisualLifecycle.GetEmitter(self: Tracker, emitterId: string): EmitterState?
	local emitter = self._emittersById[emitterId]
	if emitter == nil then
		return nil
	end
	return copyEmitter(emitter)
end

function VisualLifecycle.GetState(self: Tracker): State
	local emitters = {}
	for _, emitterId in ipairs(self._emitterOrder) do
		table.insert(emitters, copyEmitter(self._emittersById[emitterId]))
	end

	return {
		requestKind = self._requestKind,
		requestAt = self._requestAt,
		releaseAt = self._releaseAt,
		emitters = emitters,
	}
end

VisualLifecycle.New = VisualLifecycle.new

return table.freeze(VisualLifecycle)
