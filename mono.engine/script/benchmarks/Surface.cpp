// What the v0.6 script surface costs, per tick and per call.
//
// **Three things run every tick once a game is scripted**, and each of them is
// new at v0.6, so none of them has a number anybody has looked at:
//
//   - the **heartbeat fan-out**, which is `SignalTable::Fire` over however many
//     handlers a game connected,
//   - the **`.Changed` fan-out**, which is the component-to-property expansion
//     `Changes.cpp` performs for every watched instance that moved,
//   - the **codec**, which every `PublishAsync` pays.
//
// Measured through the shared machinery rather than through a VM. That is the
// honest boundary: what a `lua_pcall` costs is Luau's number, and mixing it in
// would produce a figure that says more about the interpreter than about this
// engine's design. The property write path *is* measured through the VM,
// because `part.Position = v` is the call a scene makes tens of thousands of
// times a second and its cost is the binding's.
//
// The rows to watch are the fan-out ones. A `.Changed` connection on one part
// costs a walk of that part's property list - eleven descriptors - against the
// changed component set, and the roadmap's whole objection to the naive design
// was that filtering a whole-world signal per connection would walk every
// change for every listener. These rows are what say whether the design avoided
// that.
//
// What it measured, in the `bench` preset, on a 24-thread machine. Minimum
// sample, spread beside it:
//
// | Row | Cost |
// |---|---|
// | Heartbeat, 1 connection | 5 ns |
// | Heartbeat, 64 connections | 113 ns ± 9 |
// | Heartbeat, 1024 connections | 1705 ns ± 106 |
// | Changed, 1 watched of 1000, all moved | 40.7 us ± 3.4 |
// | Changed, 100 watched of 1000, all moved | 51.6 us ± 5.7 |
// | Changed, 1000 watched of 1000, all moved | 138.0 us ± 4.9 |
// | Changed, 1000 watched, nothing moved | 1.9 us ± 0.3 |
// | Codec, a 9-field message | 244 ns ± 15 |
// | Codec, a 64-entry array | 610 ns ± 42 |
// | Task, 1000 waits scheduled and resumed | 26.0 us ± 0.5 |
// | Luau, 10000 property writes | 1255 us ± 95 |
// | Luau, 10000 property reads | 1132 us ± 128 |
//
// **The heartbeat is 1.7 nanoseconds per connection and flat**, so the fan-out
// is never the cost - whatever a handler does is.
//
// **The `.Changed` rows say the design worked.** The 40.7 us floor is the
// thousand writes and the barrier, not the fan-out: the listener fires for
// every changed entity and the watched-set lookup rejects 999 of them. What the
// fan-out itself costs is the *difference* - about 97 us for a thousand watched
// instances, or 97 ns each, which is a walk of that part's fourteen descriptors
// against the changed component. It scales with what somebody watched and not
// with what moved, which is exactly the property the roadmap objected to
// `OnChanged<T>` for lacking. The quiet row is the one most ticks take.
//
// **`Task · 1000 waits` was 5.7 milliseconds** when this suite was first run -
// 220 times slower - because `Delay` appended and then `std::sort`ed the whole
// queue. It is a binary search and an insert now. Nothing else in the tree
// would have noticed: a scene with a handful of waits pays either version
// nothing, and the cost only appears once a game schedules them in bulk.
//
// A property write is 126 ns through the VM, and a read 113 ns. Most of that is
// the descriptor scan - a linear walk comparing interned names over the
// fourteen a `Part` has - and it is the obvious thing to index if a scene ever
// makes this the frame's cost. It is not today: 512 parts writing one property
// each is 65 us against a 16 ms frame.
//
// A number is reported, never enforced. Read it, then decide.

#include "../src/Changes.hpp"
#include "../src/Codec.hpp"
#include "../src/Signals.hpp"
#include "../src/Tasks.hpp"

#include <engine/ecs/Store.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Part.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/testing/Bench.hpp>

#include <memory>
#include <string>
#include <vector>

TEST_SUITE_ID("engine.script.bench.surface")

using engine::ecs::Entity;
using engine::ecs::Store;
using engine::script::CallbackRef;
using engine::script::ChangeQueue;
using engine::script::CodecStatus;
using engine::script::Connection;
using engine::script::Encode;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::ScriptValue;
using engine::script::SignalKind;
using engine::script::SignalTable;
using engine::script::TaskQueue;
using engine::script::ValueTag;
using engine::testing::Consume;

namespace surface_bench {
	// A signal table with `count` handlers on the heartbeat.
	SignalTable &HeartbeatWith(size_t count) {
		static SignalTable table;
		static size_t built = 0;

		if (built != count) {
			std::vector<CallbackRef> released;
			table.Clear(released);
			for (size_t index = 0; index < count; index++) {
				table.Connect(
					SignalKind::Heartbeat, engine::ecs::NULL_ENTITY, static_cast<CallbackRef>(index)
				);
			}
			built = count;
		}
		return table;
	}

	// A world of `count` parts, `watched` of which carry a `.Changed`
	// connection.
	//
	// Held rather than rebuilt per sample: constructing a world inside a
	// measured body measures `CreateInstance`, which has its own suite.
	struct Scene {
		std::unique_ptr<Store> World;
		ChangeQueue Changes;
		std::vector<Entity> Parts;
	};

	Scene &SceneOf(size_t count, size_t watched) {
		static Scene scene;
		static size_t builtCount = 0;
		static size_t builtWatched = 0;

		if (builtCount == count && builtWatched == watched) {
			return scene;
		}

		engine::scene::EnsureClassTree();

		if (scene.World != nullptr) {
			scene.Changes.Detach(*scene.World);
		}

		scene.Parts.clear();
		scene.World = std::make_unique<Store>("bench.world");

		for (size_t index = 0; index < count; index++) {
			scene.Parts.push_back(scene.World->CreateInstance(engine::scene::PartClass()));
		}
		for (size_t index = 0; index < watched && index < count; index++) {
			scene.Changes.Watch(*scene.World, scene.Parts[index]);
		}

		builtCount = count;
		builtWatched = watched;
		return scene;
	}

	// One tick's worth of movement over a scene, then the barrier.
	void MoveAndFlush(Scene &scene, size_t moving) {
		scene.World->ClearChanges();
		for (size_t index = 0; index < moving && index < scene.Parts.size(); index++) {
			if (auto *transform = scene.World->GetMutable<engine::scene::Transform>(scene.Parts[index]);
				transform != nullptr) {
				transform->Frame.Position.X += 1.0f;
			}
		}
		scene.World->FlushSignals();
	}

	// A payload the size a real message is: a handful of named fields.
	ScriptValue &Message() {
		static ScriptValue value = [] {
			ScriptValue built{ValueTag::Map};
			for (int index = 0; index < 8; index++) {
				ScriptValue number{ValueTag::Number};
				number.Number = index * 1.5;
				built.Entries.emplace_back(std::string("field") + std::to_string(index), number);
			}

			ScriptValue text{ValueTag::String};
			text.Text = "a message of about the length a real one has";
			built.Entries.emplace_back("text", text);
			return built;
		}();
		return value;
	}
}

using namespace surface_bench;

// --- the heartbeat fan-out --------------------------------------------------
//
// The floor under every scripted tick. The body does nothing, so what is
// measured is the walk, the snapshot of the count and the live check - not what
// a handler costs.

BENCH("Heartbeat · fire 1 connection", 20000) {
	SignalTable &table = HeartbeatWith(1);
	for (int pass = 0; pass < 20000; pass++) {
		table.Fire(SignalKind::Heartbeat, engine::ecs::NULL_ENTITY, [](const Connection &connection) {
			Consume(connection.Callback);
		});
	}
}

BENCH("Heartbeat · fire 64 connections", 5000) {
	SignalTable &table = HeartbeatWith(64);
	for (int pass = 0; pass < 5000; pass++) {
		table.Fire(SignalKind::Heartbeat, engine::ecs::NULL_ENTITY, [](const Connection &connection) {
			Consume(connection.Callback);
		});
	}
}

BENCH("Heartbeat · fire 1024 connections", 500) {
	SignalTable &table = HeartbeatWith(1024);
	for (int pass = 0; pass < 500; pass++) {
		table.Fire(SignalKind::Heartbeat, engine::ecs::NULL_ENTITY, [](const Connection &connection) {
			Consume(connection.Callback);
		});
	}
}

BENCH("Connect and disconnect · 1000 pairs", 200) {
	for (int pass = 0; pass < 200; pass++) {
		SignalTable table;
		for (int index = 0; index < 1000; index++) {
			const auto id = table.Connect(SignalKind::Heartbeat, engine::ecs::NULL_ENTITY, index);
			CallbackRef released = 0;
			table.Disconnect(id, released);
			Consume(released);
		}
	}
}

// --- the change fan-out -----------------------------------------------------
//
// **The rows that decide whether the design worked.** The cost should scale
// with what somebody *watched*, not with what moved - that is the whole reason
// the queue filters on a set rather than walking every change per listener.

BENCH("Changed · 1 watched of 1000 parts, 1000 moved", 500) {
	Scene &scene = SceneOf(1000, 1);
	for (int pass = 0; pass < 500; pass++) {
		MoveAndFlush(scene, 1000);
		size_t fired = 0;
		scene.Changes.Drain([&](Entity, engine::core::Name) { fired++; });
		Consume(fired);
	}
}

BENCH("Changed · 100 watched of 1000 parts, 1000 moved", 500) {
	Scene &scene = SceneOf(1000, 100);
	for (int pass = 0; pass < 500; pass++) {
		MoveAndFlush(scene, 1000);
		size_t fired = 0;
		scene.Changes.Drain([&](Entity, engine::core::Name) { fired++; });
		Consume(fired);
	}
}

BENCH("Changed · 1000 watched of 1000 parts, 1000 moved", 200) {
	Scene &scene = SceneOf(1000, 1000);
	for (int pass = 0; pass < 200; pass++) {
		MoveAndFlush(scene, 1000);
		size_t fired = 0;
		scene.Changes.Drain([&](Entity, engine::core::Name) { fired++; });
		Consume(fired);
	}
}

BENCH("Changed · 1000 watched, nothing moved", 2000) {
	// The quiet case, which is most ticks. Nothing should be queued and nothing
	// should be walked.
	Scene &scene = SceneOf(1000, 1000);
	for (int pass = 0; pass < 2000; pass++) {
		MoveAndFlush(scene, 0);
		size_t fired = 0;
		scene.Changes.Drain([&](Entity, engine::core::Name) { fired++; });
		Consume(fired);
	}
}

// --- the codec --------------------------------------------------------------
//
// What every `PublishAsync` pays. The sort is part of the format, so it is part
// of the measurement.

BENCH("Codec · encode a 9-field message", 20000) {
	std::vector<std::byte> bytes;
	for (int pass = 0; pass < 20000; pass++) {
		ScriptValue value = Message();
		Consume(static_cast<int>(Encode(value, bytes)));
		Consume(bytes.size());
	}
}

BENCH("Codec · encode a 64-entry array", 10000) {
	ScriptValue array{ValueTag::Array};
	for (int index = 0; index < 64; index++) {
		ScriptValue number{ValueTag::Number};
		number.Number = index;
		array.Items.push_back(number);
	}

	std::vector<std::byte> bytes;
	for (int pass = 0; pass < 10000; pass++) {
		ScriptValue value = array;
		Consume(static_cast<int>(Encode(value, bytes)));
	}
}

// --- the task queue ---------------------------------------------------------

BENCH("Task · 1000 waits scheduled and resumed", 200) {
	for (int pass = 0; pass < 200; pass++) {
		TaskQueue queue;
		for (int index = 0; index < 1000; index++) {
			queue.Delay(index, static_cast<uint64_t>(index % 8));
		}

		size_t resumed = 0;
		queue.Advance(8, [&](CallbackRef) { resumed++; });
		Consume(resumed);
	}
}

// --- through the VM ---------------------------------------------------------
//
// The one row where an interpreter's cost belongs in the number, because
// `part.Position = v` is the call a scripted scene makes most and every part of
// it - the metatable dispatch, the descriptor lookup, the conversion, the
// change mark - is this engine's.

BENCH("Luau · 10000 property writes", 20) {
	engine::scene::EnsureClassTree();

	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.script");
		const auto runtime = MakeRuntime(store, Language::Luau);

		const bool ok = runtime->Run(R"(
			local part = Instance.new('Part')
			for index = 1, 10000 do
				part.Position = Vector3.new(index, 0, 0)
			end
		)");
		Consume(ok ? 1 : 0);
	}
}

BENCH("Luau · 10000 property reads", 20) {
	engine::scene::EnsureClassTree();

	for (int pass = 0; pass < 20; pass++) {
		Store store("bench.script");
		const auto runtime = MakeRuntime(store, Language::Luau);

		const bool ok = runtime->Run(R"(
			local part = Instance.new('Part')
			local total = 0
			for index = 1, 10000 do
				total += part.Position.X
			end
		)");
		Consume(ok ? 1 : 0);
	}
}
