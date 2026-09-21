--!strict

local DEFAULT_STEP_SECONDS = 1 / 30
local DEFAULT_MAX_CATCH_UP_STEPS = 8
local DEFAULT_MAX_QUEUED_EVENTS = 4096
local DEFAULT_MAX_BUFFERED_EVENTS = 4096
local DEFAULT_MAX_EVENTS_PER_STEP = 1024

export type Config = {
	stepSeconds: number?,
	fixedStep: number?,
	maxCatchUpSteps: number?,
	maxBacklogSeconds: number?,
	maxQueuedEvents: number?,
	maxBufferedEvents: number?,
	maxEventsPerStep: number?,
}

type ResolvedConfig = {
	stepSeconds: number,
	maxCatchUpSteps: number,
	maxBacklogSeconds: number,
	maxQueuedEvents: number,
	maxBufferedEvents: number,
	maxEventsPerStep: number,
}

export type ScheduleInput = {
	scheduledTime: number,
	kind: string,
	payload: unknown?,
}

export type ScheduledEvent = {
	sequence: number,
	scheduledTime: number,
	kind: string,
	payload: unknown?,
}

export type SchedulePredicate = (event: ScheduledEvent) -> boolean

export type ProcessedEvent = {
	sequence: number,
	scheduledTime: number,
	processedAt: number,
	kind: string,
	payload: unknown?,
}

export type AdvanceResult = {
	simulationTime: number,
	steps: number,
	processedEvents: number,
	accumulator: number,
	droppedBacklogSeconds: number,
	totalDroppedBacklogSeconds: number,
	queuedEvents: number,
	bufferedEvents: number,
	eventBackpressure: boolean,
}

export type Stats = {
	simulationTime: number,
	accumulator: number,
	nextSequence: number,
	queuedEvents: number,
	bufferedEvents: number,
	totalDroppedBacklogSeconds: number,
}

export type Solver = {
	_config: ResolvedConfig,
	_simulationTime: number,
	_accumulator: number,
	_nextSequence: number,
	_queue: { ScheduledEvent },
	_buffer: { ProcessedEvent },
	_totalDroppedBacklogSeconds: number,

	Schedule: (
		self: Solver,
		inputOrScheduledTime: ScheduleInput | number,
		kind: string?,
		payload: unknown?
	) -> (ScheduledEvent?, string?),
	Advance: (self: Solver, deltaSeconds: number) -> AdvanceResult,
	Drain: (self: Solver) -> { ProcessedEvent },
	CanSchedule: (self: Solver, count: number?) -> boolean,
	CanReplaceScheduled: (self: Solver, predicate: SchedulePredicate, replacementCount: number) -> boolean,
	ReplaceScheduled: (
		self: Solver,
		predicate: SchedulePredicate,
		replacements: { ScheduleInput }
	) -> ({ ScheduledEvent }?, string?),
	GetStats: (self: Solver) -> Stats,
}

local EventSolver = {}
EventSolver.__index = EventSolver

local function isFinite(value: number): boolean
	return value == value and math.abs(value) ~= math.huge
end

local function positiveNumber(value: number?, fallback: number, name: string): number
	local result = if value == nil then fallback else value
	assert(isFinite(result) and result > 0, name .. " must be a finite positive number")
	return result
end

local function positiveInteger(value: number?, fallback: number, name: string): number
	local result = positiveNumber(value, fallback, name)
	assert(result % 1 == 0, name .. " must be an integer")
	return result
end

local function resolveConfig(config: Config?): ResolvedConfig
	local supplied: Config = if config == nil then {} else config
	local stepSeconds = positiveNumber(supplied.stepSeconds or supplied.fixedStep, DEFAULT_STEP_SECONDS, "stepSeconds")
	local maxCatchUpSteps = positiveInteger(
		supplied.maxCatchUpSteps,
		DEFAULT_MAX_CATCH_UP_STEPS,
		"maxCatchUpSteps"
	)
	local maxBacklogSeconds = positiveNumber(
		supplied.maxBacklogSeconds,
		stepSeconds * maxCatchUpSteps,
		"maxBacklogSeconds"
	)
	assert(maxBacklogSeconds >= stepSeconds, "maxBacklogSeconds must hold at least one fixed step")

	return {
		stepSeconds = stepSeconds,
		maxCatchUpSteps = maxCatchUpSteps,
		maxBacklogSeconds = maxBacklogSeconds,
		maxQueuedEvents = positiveInteger(
			supplied.maxQueuedEvents,
			DEFAULT_MAX_QUEUED_EVENTS,
			"maxQueuedEvents"
		),
		maxBufferedEvents = positiveInteger(
			supplied.maxBufferedEvents,
			DEFAULT_MAX_BUFFERED_EVENTS,
			"maxBufferedEvents"
		),
		maxEventsPerStep = positiveInteger(
			supplied.maxEventsPerStep,
			DEFAULT_MAX_EVENTS_PER_STEP,
			"maxEventsPerStep"
		),
	}
end

local function comesBefore(a: ScheduledEvent, b: ScheduledEvent): boolean
	if a.scheduledTime ~= b.scheduledTime then
		return a.scheduledTime < b.scheduledTime
	end
	return a.sequence < b.sequence
end

local function insertOrdered(queue: { ScheduledEvent }, event: ScheduledEvent)
	local low = 1
	local high = #queue + 1

	while low < high do
		local middle = math.floor((low + high) / 2)
		local current = queue[middle]
		if current ~= nil and comesBefore(current, event) then
			low = middle + 1
		else
			high = middle
		end
	end

	table.insert(queue, low, event)
end

function EventSolver.new(config: Config?): Solver
	local self = setmetatable({
		_config = resolveConfig(config),
		_simulationTime = 0,
		_accumulator = 0,
		_nextSequence = 1,
		_queue = {},
		_buffer = {},
		_totalDroppedBacklogSeconds = 0,
	}, EventSolver)

	return (self :: unknown) :: Solver
end

function EventSolver.Schedule(
	self: Solver,
	inputOrScheduledTime: ScheduleInput | number,
	kind: string?,
	payload: unknown?
): (ScheduledEvent?, string?)
	if #self._queue >= self._config.maxQueuedEvents then
		return nil, "QueueFull"
	end

	local scheduledTime: number
	local eventKind: string
	local eventPayload: unknown?
	if type(inputOrScheduledTime) == "number" then
		scheduledTime = inputOrScheduledTime
		eventKind = kind or ""
		eventPayload = payload
	else
		scheduledTime = inputOrScheduledTime.scheduledTime
		eventKind = inputOrScheduledTime.kind
		eventPayload = inputOrScheduledTime.payload
	end

	if not isFinite(scheduledTime) then
		return nil, "InvalidScheduledTime"
	end
	if eventKind == "" then
		return nil, "InvalidKind"
	end

	local event: ScheduledEvent = {
		sequence = self._nextSequence,
		scheduledTime = scheduledTime,
		kind = eventKind,
		payload = eventPayload,
	}
	self._nextSequence += 1
	insertOrdered(self._queue, event)
	return event, nil
end

local function processDueEvents(self: Solver): (number, boolean)
	local processed = 0
	local config = self._config
	local epsilon = config.stepSeconds * 1e-9

	while processed < config.maxEventsPerStep and #self._buffer < config.maxBufferedEvents do
		local nextEvent = self._queue[1]
		if nextEvent == nil or nextEvent.scheduledTime > self._simulationTime + epsilon then
			break
		end

		table.remove(self._queue, 1)
		processed += 1
		table.insert(self._buffer, {
			sequence = nextEvent.sequence,
			scheduledTime = nextEvent.scheduledTime,
			processedAt = self._simulationTime,
			kind = nextEvent.kind,
			payload = nextEvent.payload,
		})
	end

	local nextEvent = self._queue[1]
	local hasDueEvent = nextEvent ~= nil and nextEvent.scheduledTime <= self._simulationTime + epsilon
	return processed, hasDueEvent
end

function EventSolver.Advance(self: Solver, deltaSeconds: number): AdvanceResult
	assert(isFinite(deltaSeconds) and deltaSeconds >= 0, "deltaSeconds must be finite and non-negative")

	local config = self._config
	local droppedBacklogSeconds = 0
	local accumulated = self._accumulator + deltaSeconds
	if accumulated > config.maxBacklogSeconds then
		droppedBacklogSeconds = accumulated - config.maxBacklogSeconds
		accumulated = config.maxBacklogSeconds
		self._totalDroppedBacklogSeconds += droppedBacklogSeconds
	end
	self._accumulator = accumulated

	local availableSteps = math.floor((self._accumulator + config.stepSeconds * 1e-9) / config.stepSeconds)
	local stepCount = math.min(availableSteps, config.maxCatchUpSteps)
	local processedEvents = 0
	local eventBackpressure = false

	for _ = 1, stepCount do
		self._simulationTime += config.stepSeconds
		self._accumulator -= config.stepSeconds
		if self._accumulator < 0 and self._accumulator > -config.stepSeconds * 1e-9 then
			self._accumulator = 0
		end

		local stepProcessed, hasDueEvent = processDueEvents(self)
		processedEvents += stepProcessed
		if hasDueEvent then
			eventBackpressure = true
		end
	end

	return {
		simulationTime = self._simulationTime,
		steps = stepCount,
		processedEvents = processedEvents,
		accumulator = self._accumulator,
		droppedBacklogSeconds = droppedBacklogSeconds,
		totalDroppedBacklogSeconds = self._totalDroppedBacklogSeconds,
		queuedEvents = #self._queue,
		bufferedEvents = #self._buffer,
		eventBackpressure = eventBackpressure,
	}
end

function EventSolver.Drain(self: Solver): { ProcessedEvent }
	local drained = self._buffer
	self._buffer = {}
	return drained
end

function EventSolver.CanSchedule(self: Solver, count: number?): boolean
	local requested = count or 1
	return requested >= 0
		and requested % 1 == 0
		and #self._queue + requested <= self._config.maxQueuedEvents
end

-- Reports whether one atomic replacement can fit after matching queued work is removed.
function EventSolver.CanReplaceScheduled(
	self: Solver,
	predicate: SchedulePredicate,
	replacementCount: number
): boolean
	if replacementCount < 0 or replacementCount % 1 ~= 0 then
		return false
	end
	local removedCount = 0
	for _, event in ipairs(self._queue) do
		if predicate(event) then
			removedCount += 1
		end
	end
	return #self._queue - removedCount + replacementCount <= self._config.maxQueuedEvents
end

-- Replaces matching queued work without exposing a remove-then-schedule failure window.
-- Invalid input or insufficient capacity leaves the queue and sequence unchanged.
function EventSolver.ReplaceScheduled(
	self: Solver,
	predicate: SchedulePredicate,
	replacements: { ScheduleInput }
): ({ ScheduledEvent }?, string?)
	for _, replacement in ipairs(replacements) do
		if not isFinite(replacement.scheduledTime) then
			return nil, "InvalidScheduledTime"
		end
		if replacement.kind == "" then
			return nil, "InvalidKind"
		end
	end
	if not self:CanReplaceScheduled(predicate, #replacements) then
		return nil, "QueueFull"
	end

	local nextQueue: { ScheduledEvent } = {}
	for _, event in ipairs(self._queue) do
		if not predicate(event) then
			table.insert(nextQueue, event)
		end
	end

	local nextSequence = self._nextSequence
	local scheduled = {}
	for _, replacement in ipairs(replacements) do
		local event: ScheduledEvent = {
			sequence = nextSequence,
			scheduledTime = replacement.scheduledTime,
			kind = replacement.kind,
			payload = replacement.payload,
		}
		nextSequence += 1
		insertOrdered(nextQueue, event)
		table.insert(scheduled, event)
	end

	self._queue = nextQueue
	self._nextSequence = nextSequence
	return scheduled, nil
end

function EventSolver.GetStats(self: Solver): Stats
	return {
		simulationTime = self._simulationTime,
		accumulator = self._accumulator,
		nextSequence = self._nextSequence,
		queuedEvents = #self._queue,
		bufferedEvents = #self._buffer,
		totalDroppedBacklogSeconds = self._totalDroppedBacklogSeconds,
	}
end

EventSolver.DEFAULT_STEP_SECONDS = DEFAULT_STEP_SECONDS
EventSolver.New = EventSolver.new

return table.freeze(EventSolver)
