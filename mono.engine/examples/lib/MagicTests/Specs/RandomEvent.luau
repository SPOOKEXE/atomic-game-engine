--!strict

return function(harness: any, core: any)
	local EventSolver = core.EventSolver
	local RandomStreams = core.RandomStreams

	harness:suite("random streams", function(h: any)
		h:test("derived seeds match golden vectors", function()
			h:assertEqual(RandomStreams.deriveSeed(12345, "node.fire", 1, "entity.7", 2, "burst.direction"), 684931596)
			h:assertEqual(RandomStreams.deriveSeed(0, "trajectory", 0), 1157435595)
			h:assertEqual(RandomStreams.deriveSeed(-7, true, "x"), 663660293)
		end)

		h:test("same derived stream repeats draws", function()
			local first = RandomStreams.new(42, "spread", 3)
			local second = RandomStreams.new(42, "spread", 3)
			for _ = 1, 8 do
				h:assertEqual(first:NextInteger(-1000, 1000), second:NextInteger(-1000, 1000))
			end
		end)

		h:test("purpose labels isolate streams", function()
			h:assertEqual(RandomStreams.deriveSeed(42, "purpose.a", 1), 641857774)
			h:assertEqual(RandomStreams.deriveSeed(42, "purpose.b", 1), 1461236100)
			h:assertTrue(RandomStreams.deriveSeed(42, "purpose.a", 1) ~= RandomStreams.deriveSeed(42, "purpose.b", 1))
		end)

		h:test("child index isolates streams", function()
			h:assertEqual(RandomStreams.deriveSeed(42, "purpose.a", 2), 1248127089)
			h:assertTrue(RandomStreams.deriveSeed(42, "purpose.a", 1) ~= RandomStreams.deriveSeed(42, "purpose.a", 2))
		end)

		h:test("typed keys preserve primitive identity and reject unsafe numbers", function()
			h:assertEqual(
				RandomStreams.deriveSeed(9, RandomStreams.typed("string", "1")),
				RandomStreams.deriveSeed(9, "1")
			)
			h:assertTrue(RandomStreams.deriveSeed(9, "1") ~= RandomStreams.deriveSeed(9, 1))
			h:assertThrows(function()
				RandomStreams.deriveSeed(0 / 0, "bad")
			end, "finite safe integer")
			h:assertThrows(function()
				RandomStreams.deriveSeed(9, "before", (nil :: any), "after")
			end, "stable keys")
		end)
	end)

	harness:suite("fixed-step event solver", function(h: any)
		h:test("same-time ties keep schedule sequence", function()
			local solver = EventSolver.new({ stepSeconds = 0.1 })
			solver:Schedule(0.1, "first")
			solver:Schedule(0.1, "second")
			solver:Advance(0.1)
			local events = solver:Drain()
			h:assertEqual(events[1].kind, "first")
			h:assertEqual(events[2].kind, "second")
			h:assertTrue(events[1].sequence < events[2].sequence)
		end)

		h:test("delayed events wait for their due step", function()
			local solver = EventSolver.new({ stepSeconds = 0.1 })
			solver:Schedule(0.2, "delayed")
			solver:Advance(0.1)
			h:assertEqual(#solver:Drain(), 0)
			solver:Advance(0.1)
			h:assertEqual(solver:Drain()[1].kind, "delayed")
		end)

		h:test("late work keeps scheduled and processed times separate", function()
			local solver = EventSolver.new({ stepSeconds = 0.1 })
			solver:Schedule(0.15, "late")
			solver:Advance(0.2)
			local event = solver:Drain()[1]
			h:assertNear(event.scheduledTime, 0.15)
			h:assertNear(event.processedAt, 0.2)
		end)

		h:test("catch-up and backlog are both bounded", function()
			local solver = EventSolver.new({
				stepSeconds = 0.1,
				maxCatchUpSteps = 2,
				maxBacklogSeconds = 0.3,
			})
			local result = solver:Advance(1)
			h:assertEqual(result.steps, 2)
			h:assertNear(result.simulationTime, 0.2)
			h:assertNear(result.accumulator, 0.1)
			h:assertNear(result.droppedBacklogSeconds, 0.7)
		end)

		h:test("queue cap rejects excess events", function()
			local solver = EventSolver.new({ maxQueuedEvents = 2 })
			h:assertTrue(solver:CanSchedule(2))
			solver:Schedule(1, "one")
			solver:Schedule(1, "two")
			local event, reason = solver:Schedule(1, "three")
			h:assertNil(event)
			h:assertEqual(reason, "QueueFull")
			h:assertEqual(solver:CanSchedule(1), false)
		end)

		h:test("per-step event cap reports backpressure", function()
			local solver = EventSolver.new({ stepSeconds = 0.1, maxEventsPerStep = 1 })
			solver:Schedule(0.1, "one")
			solver:Schedule(0.1, "two")
			local result = solver:Advance(0.1)
			h:assertEqual(result.processedEvents, 1)
			h:assertTrue(result.eventBackpressure)
			h:assertEqual(result.queuedEvents, 1)
		end)

		h:test("invalid schedule values return stable reasons", function()
			local solver = EventSolver.new()
			local _, timeReason = solver:Schedule(0 / 0, "bad")
			local _, kindReason = solver:Schedule(0, "")
			h:assertEqual(timeReason, "InvalidScheduledTime")
			h:assertEqual(kindReason, "InvalidKind")
		end)
	end)
end
