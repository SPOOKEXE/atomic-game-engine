#include "RendererState.hpp"

#include <algorithm>
#include <utility>

namespace engine::render {
	WeatherTemporalHistory &
	Renderer::Impl::WeatherTemporalAt(size_t slot, uint64_t world, core::Name pipeline) {
		for (auto &target : WeatherTemporalTargets) {
			if (target.Slot != slot) continue;
			if (target.World != world || target.Pipeline != pipeline) {
				ReleaseWeatherTemporalHistory(Device, target.History);
				target.World = world;
				target.Pipeline = pipeline;
			}
			return target.History;
		}
		WeatherTemporalTarget target;
		target.Slot = slot;
		target.World = world;
		target.Pipeline = pipeline;
		WeatherTemporalTargets.push_back(std::move(target));
		return WeatherTemporalTargets.back().History;
	}

	void Renderer::Impl::ReleaseWeatherTemporalSlot(size_t slot) {
		std::erase_if(WeatherTemporalTargets, [this, slot](auto &target) {
			if (target.Slot != slot) return false;
			ReleaseWeatherTemporalHistory(Device, target.History);
			return true;
		});
	}

	void Renderer::Impl::ForgetWeatherTemporalWorld(uint64_t world) {
		std::erase_if(WeatherTemporalTargets, [this, world](auto &target) {
			if (target.World != world) return false;
			ReleaseWeatherTemporalHistory(Device, target.History);
			return true;
		});
	}

	void Renderer::Impl::ReleaseWeatherTemporalHistories() {
		for (auto &target : WeatherTemporalTargets)
			ReleaseWeatherTemporalHistory(Device, target.History);
		WeatherTemporalTargets.clear();
	}

	void Renderer::Impl::CommitWeatherTemporalWrites(const void *command) {
		for (auto &target : WeatherTemporalTargets)
			target.History.Commit(command);
	}

	void Renderer::Impl::DiscardWeatherTemporalWrites(const void *command) {
		for (auto &target : WeatherTemporalTargets)
			target.History.Discard(command);
	}
}
