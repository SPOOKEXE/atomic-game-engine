#include <engine/core/HeapProfile.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/gui/Components.hpp>
#include <engine/gui/PlayerGui.hpp>
#include <engine/gui/Registration.hpp>
#include <engine/scene/EditableImage.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/scene/Services.hpp>
#include <engine/testing/Bench.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>

TEST_SUITE_ID("engine.script.bench.playergui")

namespace {
	using namespace engine;
	constexpr size_t REFRESHES = 100;
	constexpr size_t PIXEL_BYTES = 1920 * 1080 * 4;

	struct Scenario {
		ecs::Store Store;
		ecs::Entity Player;
		ecs::Entity Source;
		ecs::Entity Copy;
		ecs::Entity Image;
		bool WithImage;

		explicit Scenario(bool image) : Store(image ? "gui-bench-image" : "gui-bench-hud"), WithImage(image) {
			scene::RegisterSceneClasses();
			gui::RegisterGuiClasses();
			scene::InstallServices(Store);
			Player = scene::AddPlayer(Store, "Ada");
			Source = Store.CreateInstance(gui::GuiClass("ScreenGui"), "Hud");
			Store.SetParent(Source, Store.FindFirstChild(Player, "PlayerGui"));
			for (size_t index = 0; index < 32; ++index) {
				const auto label = Store.CreateInstance(gui::GuiClass("TextLabel"), "Status");
				Store.SetParent(label, Source);
				Store.GetMutable<gui::Label>(label)->Text = "ordinary retained HUD";
			}
			if (image) {
				Image = Store.CreateInstance(scene::EditableImageClass(), "Pixels");
				Store.SetParent(Image, Source);
				auto *pixels = Store.GetMutable<scene::EditableImage>(Image);
				pixels->Width = 1920;
				pixels->Height = 1080;
				pixels->Pixels.assign(PIXEL_BYTES, uint8_t{0x7F});
			}
			Measure("first", 1);
			Copy = gui::FindPlayerGuiCopy(Store, Source);
			if (Copy == ecs::NULL_ENTITY) throw std::runtime_error("GUI projection was not created");
			if (image) {
				const auto localImage = gui::FindPlayerGuiCopy(Store, Image);
				const auto *pixels = Store.Get<scene::EditableImage>(localImage);
				if (pixels == nullptr || pixels->Pixels.size() != PIXEL_BYTES)
					throw std::runtime_error("GUI image payload was not copied");
			}
		}

		// Setup is outside the cold measurement. Heap totals cover actual C++
		// allocations made by the same public refresh used in client hosts.
		void Measure(const char *phase, size_t calls) {
			const bool report = std::getenv("MONO_PLAYERGUI_PROJECTION_REPORT") != nullptr;
			if (report && !core::HeapProfile::IsCompiledIn())
				throw std::runtime_error("GUI projection allocation report requires heap hooks");
			const auto before = core::HeapProfile::Totals();
			const auto start = std::chrono::steady_clock::now();
			for (size_t call = 0; call < calls; ++call)
				testing::Consume(gui::RefreshPlayerGuiProjection(Store, Player, ecs::NULL_ENTITY));
			const auto end = std::chrono::steady_clock::now();
			const auto after = core::HeapProfile::Totals();
			if (report) {
				std::printf(
					"# playergui phase=%s scenario=%s calls=%zu source_pixel_bytes=%zu "
					"ns_per_call=%.3f allocated_bytes_per_call=%.3f allocations_per_call=%.3f "
					"live_bytes_delta=%lld heap_coverage=cxx_new_delete\n",
					phase,
					WithImage ? "hud-plus-1080p-image" : "hud-32-labels",
					calls,
					WithImage ? PIXEL_BYTES : size_t{0},
					std::chrono::duration<double, std::nano>(end - start).count() / calls,
					static_cast<double>(after.TotalBytes - before.TotalBytes) / calls,
					static_cast<double>(after.TotalBlocks - before.TotalBlocks) / calls,
					static_cast<long long>(after.LiveBytes - before.LiveBytes)
				);
			}
		}
	};

	Scenario &Fixture(bool image) {
		static std::unique_ptr<Scenario> hud, pixels;
		auto &fixture = image ? pixels : hud;
		if (fixture == nullptr) fixture = std::make_unique<Scenario>(image);
		return *fixture;
	}
}

BENCH("PlayerGui unchanged 32-label HUD", REFRESHES) {
	Fixture(false).Measure("unchanged", REFRESHES);
}

BENCH("PlayerGui unchanged HUD plus 1080p EditableImage", REFRESHES) {
	Fixture(true).Measure("unchanged", REFRESHES);
}
