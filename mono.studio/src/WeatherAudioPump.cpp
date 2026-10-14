#include "WeatherAudio.hpp"

#include <studio/Editor.hpp>

namespace studio {
	void Editor::PumpWeatherAudio() {
		// Headless authoring never opens a device. Data-factory audio belongs to
		// its dedicated observation host and keeps its declared sample schedule.
		if (Settings.Headless || FactoryHost || !Universe || !AnyRunning()) {
			if (WeatherAudio_) WeatherAudio_->Silence();
			return;
		}
		const WorldId shown = ViewportWorld(FocusedViewport);
		if (!shown.IsValid() || (!RunOf(shown) && !RunForReplica(shown))) {
			if (WeatherAudio_) WeatherAudio_->Silence();
			return;
		}
		const WorldId world = VisualWorldOf(shown);
		const auto *viewport = ExtraAt(FocusedViewport);
		const auto listener = viewport ? viewport->Frame.Position : CameraFrame.Position;
		if (!WeatherAudio_) WeatherAudio_ = std::make_unique<WeatherAudio>();
		Universe->Enter(world, [&](engine::ecs::Store &store) { WeatherAudio_->Sync(store, listener); });
	}
}
