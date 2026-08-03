#include "../src/Signals.hpp"

#include "../src/Tasks.hpp"

#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_SUITE_ID("engine.script.signals")

using engine::ecs::Entity;
using engine::ecs::NULL_ENTITY;
using engine::script::CallbackRef;
using engine::script::Connection;
using engine::script::ConnectionId;
using engine::script::SignalKind;
using engine::script::SignalTable;
using engine::script::TaskQueue;

namespace {
	// The callables a VM would own. These tests are the *ordering* rules, which
	// are what both VMs share, so a `CallbackRef` here is just a number.
	std::vector<CallbackRef> FiredOn(SignalTable &table, SignalKind kind, Entity subject) {
		std::vector<CallbackRef> fired;
		table.Fire(kind, subject, [&](const Connection &connection) {
			fired.push_back(connection.Callback);
		});
		return fired;
	}
}

TEST_CASE("connections fire in the order they were made", "[signals]") {
	// Insertion order and nothing else. A hash walk would make the sequence a
	// script sees depend on pointer values, and two runs of one script must
	// call the same functions in the same order.
	SignalTable table;
	for (CallbackRef reference = 10; reference < 15; reference++) {
		table.Connect(SignalKind::Heartbeat, NULL_ENTITY, reference);
	}

	CHECK(FiredOn(table, SignalKind::Heartbeat, NULL_ENTITY) == std::vector<CallbackRef>{10, 11, 12, 13, 14});
}

TEST_CASE("a connection made during a fire does not fire in that pass", "[signals]") {
	// The count is snapshotted on entry. Without that, a callback that connects
	// another one could extend its own iteration without bound, and how far it
	// got would depend on where the vector happened to reallocate.
	SignalTable table;
	table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 1);

	std::vector<CallbackRef> fired;
	table.Fire(SignalKind::Heartbeat, NULL_ENTITY, [&](const Connection &connection) {
		fired.push_back(connection.Callback);
		if (connection.Callback == 1) {
			table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 2);
		}
	});

	CHECK(fired == std::vector<CallbackRef>{1});

	// And it fires on the next pass, which is the other half of the contract.
	CHECK(FiredOn(table, SignalKind::Heartbeat, NULL_ENTITY) == std::vector<CallbackRef>{1, 2});
}

TEST_CASE("a disconnection during a fire takes effect immediately", "[signals]") {
	// The opposite of a connection, and deliberately: a handler that
	// disconnected a later one meant it, and calling it anyway would be running
	// something a script had just cancelled.
	SignalTable table;
	const ConnectionId first = table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 1);
	const ConnectionId second = table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 2);
	table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 3);

	std::vector<CallbackRef> fired;
	table.Fire(SignalKind::Heartbeat, NULL_ENTITY, [&](const Connection &connection) {
		fired.push_back(connection.Callback);
		if (connection.Id == first) {
			CallbackRef released = 0;
			CHECK(table.Disconnect(second, released));
			CHECK(released == 2);
		}
	});

	CHECK(fired == std::vector<CallbackRef>{1, 3});
}

TEST_CASE("a handle is never reused, so a stale disconnect kills nothing", "[signals]") {
	// A double `:Disconnect` is ordinary in real code, because a cleanup path
	// runs whether or not something else already ran. With reused ids the
	// second call would kill whatever took the slot.
	SignalTable table;
	const ConnectionId first = table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 1);

	CallbackRef released = 0;
	CHECK(table.Disconnect(first, released));
	CHECK_FALSE(table.Disconnect(first, released));

	const ConnectionId second = table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 2);
	CHECK(second != first);

	CHECK_FALSE(table.Disconnect(first, released));
	CHECK(table.Connected(second));
	CHECK(FiredOn(table, SignalKind::Heartbeat, NULL_ENTITY) == std::vector<CallbackRef>{2});
}

TEST_CASE("Connected reports what it should", "[signals]") {
	SignalTable table;
	const ConnectionId id = table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 1);

	CHECK(table.Connected(id));

	CallbackRef released = 0;
	table.Disconnect(id, released);
	CHECK_FALSE(table.Connected(id));
	CHECK_FALSE(table.Connected(9999));
}

TEST_CASE("Once is a flag the fire honours", "[signals]") {
	SignalTable table;
	const ConnectionId id = table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 7);
	CHECK(table.MarkOnce(id));

	bool sawOnce = false;
	table.Fire(SignalKind::Heartbeat, NULL_ENTITY, [&](const Connection &connection) {
		sawOnce = connection.Once;
	});
	CHECK(sawOnce);
}

TEST_CASE("signals are per subject", "[signals]") {
	SignalTable table;
	const Entity first{1};
	const Entity second{2};

	table.Connect(SignalKind::Changed, first, 10);
	table.Connect(SignalKind::Changed, second, 20);

	CHECK(FiredOn(table, SignalKind::Changed, first) == std::vector<CallbackRef>{10});
	CHECK(FiredOn(table, SignalKind::Changed, second) == std::vector<CallbackRef>{20});

	// And a kind on one subject is not the same signal as another kind on it.
	CHECK(FiredOn(table, SignalKind::PropertyChanged, first).empty());
}

TEST_CASE("subjects are visited in first-connection order", "[signals]") {
	// Not hash order. `.Changed` fires per instance, so this is the order a
	// script sees its world change in — and a hash walk would make that depend
	// on pointer values.
	SignalTable table;
	for (uint64_t id = 5; id > 0; id--) {
		table.Connect(SignalKind::Changed, Entity{id}, static_cast<CallbackRef>(id));
	}

	std::vector<uint64_t> visited;
	table.EachSubject(SignalKind::Changed, [&](Entity subject) { visited.push_back(subject.Id); });

	CHECK(visited == std::vector<uint64_t>{5, 4, 3, 2, 1});
}

TEST_CASE("a subject with no live connections is not visited", "[signals]") {
	SignalTable table;
	const Entity subject{1};
	const ConnectionId id = table.Connect(SignalKind::Changed, subject, 1);

	CallbackRef released = 0;
	table.Disconnect(id, released);

	size_t visits = 0;
	table.EachSubject(SignalKind::Changed, [&](Entity) { visits++; });
	CHECK(visits == 0);
}

TEST_CASE("dropping a subject releases everything and forgets the walk order", "[signals]") {
	// What a `:Destroy` does. A `.Changed` connection on a destroyed row would
	// otherwise fire against a dead handle every tick for the rest of the
	// world's life.
	SignalTable table;
	const Entity subject{1};
	table.Connect(SignalKind::Changed, subject, 10);
	table.Connect(SignalKind::PropertyChanged, subject, 11);
	table.Connect(SignalKind::Changed, Entity{2}, 20);

	std::vector<CallbackRef> released;
	table.DropSubject(subject, released);

	CHECK(released.size() == 2);
	CHECK(table.Count(SignalKind::Changed, subject) == 0);
	CHECK(table.Count(SignalKind::Changed, Entity{2}) == 1);

	std::vector<uint64_t> visited;
	table.EachSubject(SignalKind::Changed, [&](Entity entity) { visited.push_back(entity.Id); });
	CHECK(visited == std::vector<uint64_t>{2});
}

TEST_CASE("Clear hands back everything so a VM can release it", "[signals]") {
	SignalTable table;
	table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 1);
	table.Connect(SignalKind::Changed, Entity{1}, 2);

	std::vector<CallbackRef> released;
	table.Clear(released);

	CHECK(released.size() == 2);
	CHECK(table.Count(SignalKind::Heartbeat, NULL_ENTITY) == 0);
}

TEST_CASE("a nested fire does not compact the list the outer one is walking", "[signals]") {
	SignalTable table;
	const ConnectionId first = table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 1);
	table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 2);
	table.Connect(SignalKind::Heartbeat, NULL_ENTITY, 3);

	std::vector<CallbackRef> outer;
	table.Fire(SignalKind::Heartbeat, NULL_ENTITY, [&](const Connection &connection) {
		outer.push_back(connection.Callback);
		if (connection.Id == first) {
			CallbackRef released = 0;
			table.Disconnect(first, released);

			// A fire inside a fire. The inner one must not compact, because the
			// outer walk is holding an index into the same list.
			table.Fire(SignalKind::Heartbeat, NULL_ENTITY, [](const Connection &) {});
		}
	});

	CHECK(outer == std::vector<CallbackRef>{1, 2, 3});
}

// --- the task queue ---------------------------------------------------------

TEST_CASE("waits resume on the tick they are due and not before", "[signals][task]") {
	TaskQueue queue;
	queue.Delay(1, 10);
	queue.Delay(2, 12);

	std::vector<CallbackRef> resumed;
	const auto collect = [&](CallbackRef thread) { resumed.push_back(thread); };

	queue.Advance(9, collect);
	CHECK(resumed.empty());

	queue.Advance(10, collect);
	CHECK(resumed == std::vector<CallbackRef>{1});

	queue.Advance(11, collect);
	CHECK(resumed == std::vector<CallbackRef>{1});

	queue.Advance(12, collect);
	CHECK(resumed == std::vector<CallbackRef>{1, 2});
}

TEST_CASE("a tick that is overdue resumes everything up to it", "[signals][task]") {
	// A world that ticked several times without pumping — a host catching up —
	// must not leave a script waiting forever on a tick it stepped over.
	TaskQueue queue;
	queue.Delay(1, 5);
	queue.Delay(2, 6);
	queue.Delay(3, 100);

	std::vector<CallbackRef> resumed;
	queue.Advance(50, [&](CallbackRef thread) { resumed.push_back(thread); });

	CHECK(resumed == std::vector<CallbackRef>{1, 2});
	CHECK(queue.Pending() == 1);
}

TEST_CASE("ties break on scheduling order", "[signals][task]") {
	// Two scripts waiting the same number of ticks resume in the order they
	// asked. A heap ordered on the tick alone would leave that to whichever way
	// the comparison happened to fall.
	TaskQueue queue;
	for (CallbackRef thread = 1; thread <= 5; thread++) {
		queue.Delay(thread, 7);
	}

	std::vector<CallbackRef> resumed;
	queue.Advance(7, [&](CallbackRef thread) { resumed.push_back(thread); });

	CHECK(resumed == std::vector<CallbackRef>{1, 2, 3, 4, 5});
}

TEST_CASE("a resume that reschedules does not run twice in one pass", "[signals][task]") {
	// `while true do task.wait() end` is the shape. Appending to the vector
	// being walked would make it an infinite loop inside one tick.
	TaskQueue queue;
	queue.Delay(1, 5);

	int resumes = 0;
	queue.Advance(5, [&](CallbackRef thread) {
		resumes++;
		queue.Delay(thread, 5);
	});

	CHECK(resumes == 1);
	CHECK(queue.Pending() == 1);
}

TEST_CASE("deferred work drains in order and only once", "[signals][task]") {
	TaskQueue queue;
	queue.Defer(1);
	queue.Defer(2);

	std::vector<CallbackRef> resumed;
	queue.DrainDeferred([&](CallbackRef thread) {
		resumed.push_back(thread);
		if (thread == 1) {
			// Belongs to the next pass, not this one — otherwise a script could
			// spin the host without ever yielding a tick.
			queue.Defer(3);
		}
	});

	CHECK(resumed == std::vector<CallbackRef>{1, 2});
	CHECK(queue.Pending() == 1);
}

TEST_CASE("cancel finds a thread in either queue", "[signals][task]") {
	TaskQueue queue;
	queue.Delay(1, 5);
	queue.Defer(2);

	CHECK(queue.Cancel(1));
	CHECK(queue.Cancel(2));
	CHECK_FALSE(queue.Cancel(3));
	CHECK(queue.Pending() == 0);
}

TEST_CASE("clearing hands back every scheduled thread", "[signals][task]") {
	TaskQueue queue;
	queue.Delay(1, 5);
	queue.Delay(2, 6);
	queue.Defer(3);

	std::vector<CallbackRef> released;
	queue.Clear(released);

	CHECK(released.size() == 3);
	CHECK(queue.Pending() == 0);
}
