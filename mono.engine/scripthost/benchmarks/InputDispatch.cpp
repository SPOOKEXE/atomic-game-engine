// Measures the host input pump on active input frames with no keyboard edges
// and with sparse keyboard edges, through both VM adapters.

#include <engine/ecs/Store.hpp>
#include <engine/physics/Pipeline.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Input.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/Runtime.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Bench.hpp>

#include <array>
#include <memory>
#include <span>

TEST_SUITE_ID("engine.scripthost.bench.input-dispatch")

using engine::ecs::Store;
using engine::script::Language;
using engine::script::MakeRuntime;
using engine::script::Runtime;
using engine::testing::Consume;

namespace {
	constexpr size_t FRAME_COUNT = 1024;

	struct InputFrame {
		uint64_t Keys = 0;
		float MouseDelta = 0.0f;
	};

	using InputSequence = std::array<InputFrame, FRAME_COUNT>;

	InputSequence Sequence(bool sparseKeyEdges) {
		InputSequence frames{};
		for (size_t index = 0; index < frames.size(); index++) {
			frames[index].MouseDelta = (index & 1u) == 0 ? -1.0f : 1.0f;
			if (sparseKeyEdges && index % 128 == 31) {
				frames[index].Keys = 1ull << static_cast<size_t>(engine::scene::KeyCode::Q);
			}
		}
		return frames;
	}

	const InputSequence &ActiveFrames() {
		static const InputSequence frames = Sequence(false);
		return frames;
	}

	const InputSequence &SparseFrames() {
		static const InputSequence frames = Sequence(true);
		return frames;
	}

	struct RuntimeFixture {
		Store World;
		std::unique_ptr<Runtime> Vm;

		RuntimeFixture(const char *name, Language language) : World(name) {
			World.SetResource(engine::scene::InputState{});
			Vm = MakeRuntime(World, language);
		}

		bool Run(std::span<const InputFrame> frames) {
			if (Vm == nullptr) {
				return false;
			}

			auto *input = World.ResourceMutable<engine::scene::InputState>();
			for (const InputFrame &frame : frames) {
				input->Previous = input->Down;
				input->Down.Words[0] = frame.Keys;
				input->MouseDelta = engine::core::Vector2{frame.MouseDelta, 0.0f};
				if (!Vm->Heartbeat(1.0f / 60.0f)) {
					return false;
				}
			}
			return true;
		}
	};

	RuntimeFixture &Fixture(Language language) {
		static std::array<std::unique_ptr<RuntimeFixture>, 2> fixtures;
		const size_t index = language == Language::Luau ? 0 : 1;
		if (fixtures[index] == nullptr) {
			engine::scene::EnsureClassTree();
			engine::scene::RegisterSceneComponents();
			engine::physics::RegisterPhysicsClasses();
			fixtures[index] = std::make_unique<RuntimeFixture>(
				language == Language::Luau ? "bench.input.luau" : "bench.input.javascript", language
			);
		}
		return *fixtures[index];
	}
}

BENCH_PER_ITEM("Luau active input frames without keyboard edges", FRAME_COUNT) {
	Consume(Fixture(Language::Luau).Run(ActiveFrames()));
}

BENCH_PER_ITEM("JavaScript active input frames without keyboard edges", FRAME_COUNT) {
	Consume(Fixture(Language::JavaScript).Run(ActiveFrames()));
}

BENCH_PER_ITEM("Luau active input frames with sparse keyboard edges", FRAME_COUNT) {
	Consume(Fixture(Language::Luau).Run(SparseFrames()));
}

BENCH_PER_ITEM("JavaScript active input frames with sparse keyboard edges", FRAME_COUNT) {
	Consume(Fixture(Language::JavaScript).Run(SparseFrames()));
}
