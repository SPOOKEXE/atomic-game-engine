#pragma once

#include <engine/render/PresentationDamage.hpp>
#include <engine/render/Renderer.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <imgui.h>
#include <span>
#include <vector>

namespace studio::frame_graph_detail {

	inline constexpr double CACHE_DISPLAY_SECONDS = 0.25;

	template <class View>
	bool SampleCacheProfile(
		View &view, const engine::render::PresentationCacheProfile &profile, size_t viewport, double now
	) {
		if (viewport == view.SampledCacheViewport && now < view.NextCacheProfile &&
			view.NextCacheProfile - now <= CACHE_DISPLAY_SECONDS)
			return false;
		view.CacheProfile = profile;
		view.CacheProfileRevision++;
		view.SampledCacheViewport = viewport;
		view.NextCacheProfile = now + CACHE_DISPLAY_SECONDS;
		return true;
	}

	struct DamageFlag {
		const char *Name;
		bool engine::render::PresentationDamage::*Member;
		bool Host = false;
	};

	inline constexpr std::array DAMAGE_FLAGS{
		DamageFlag{"Scene", &engine::render::PresentationDamage::Scene},
		DamageFlag{"Objects", &engine::render::PresentationDamage::Objects},
		DamageFlag{"Particles", &engine::render::PresentationDamage::Particles},
		DamageFlag{"Environment", &engine::render::PresentationDamage::Environment},
		DamageFlag{"Portals", &engine::render::PresentationDamage::Portals},
		DamageFlag{"GameInterface", &engine::render::PresentationDamage::GameInterface},
		DamageFlag{"Viewport", &engine::render::PresentationDamage::Viewport},
		DamageFlag{"Overlay", &engine::render::PresentationDamage::Overlay},
		DamageFlag{"HostInterface", &engine::render::PresentationDamage::HostInterface, true},
	};

	struct PassFlag {
		const char *Name;
		bool engine::render::PassActivity::*Member;
	};

	inline constexpr std::array PASS_FLAGS{
		PassFlag{"SkyActive", &engine::render::PassActivity::Sky},
		PassFlag{"FogActive", &engine::render::PassActivity::Fog},
		PassFlag{"ShaderLensesActive", &engine::render::PassActivity::ShaderLenses},
		PassFlag{"DepthOfFieldActive", &engine::render::PassActivity::DepthOfField},
		PassFlag{"GodRaysActive", &engine::render::PassActivity::GodRays},
		PassFlag{"BloomActive", &engine::render::PassActivity::Bloom},
		PassFlag{"PortalOverlayActive", &engine::render::PassActivity::PortalOverlay},
		PassFlag{"MirrorOverlayActive", &engine::render::PassActivity::MirrorOverlay},
		PassFlag{"TransparentActive", &engine::render::PassActivity::Transparent},
		PassFlag{"GridActive", &engine::render::PassActivity::Grid},
		PassFlag{"ParticlesActive", &engine::render::PassActivity::Particles},
		PassFlag{"RibbonsActive", &engine::render::PassActivity::Ribbons},
	};

	inline bool HostCache(engine::render::PresentationCacheLayer layer) {
		return layer == engine::render::PresentationCacheLayer::HostInterface ||
			   layer == engine::render::PresentationCacheLayer::StudioComposition ||
			   layer == engine::render::PresentationCacheLayer::FinalImage;
	}
	struct CacheSort {
		int Column = 0;
		ImGuiSortDirection Direction = ImGuiSortDirection_None;
	};

	inline std::string_view CacheDecisionName(engine::render::PresentationCacheActivity::Decision decision) {
		using Decision = engine::render::PresentationCacheActivity::Decision;
		switch (decision) {
		case Decision::NotObserved:
			return "not seen";
		case Decision::NotApplicable:
			return "n/a";
		case Decision::Hit:
			return "hit";
		case Decision::Write:
			return "write";
		}
		return "not seen";
	}

	inline void BuildCacheOrder(
		std::span<const engine::render::PresentationCacheActivity> activities,
		bool host,
		const CacheSort *sort,
		std::vector<size_t> &order
	) {
		order.clear();
		for (size_t index = 0; index < activities.size(); index++) {
			if (HostCache(static_cast<engine::render::PresentationCacheLayer>(index)) == host)
				order.push_back(index);
		}
		if (sort == nullptr || sort->Direction == ImGuiSortDirection_None || sort->Column < 0 ||
			sort->Column >= 5)
			return;
		std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
			const auto &a = activities[left];
			const auto &b = activities[right];
			int comparison = 0;
			switch (sort->Column) {
			case 0:
				comparison = engine::render::PRESENTATION_CACHE_LAYERS[left].Name.compare(
					engine::render::PRESENTATION_CACHE_LAYERS[right].Name
				);
				break;
			case 1:
				comparison = CacheDecisionName(a.Last).compare(CacheDecisionName(b.Last));
				break;
			case 2:
				comparison = a.Hits < b.Hits ? -1 : a.Hits > b.Hits ? 1 : 0;
				break;
			case 3:
				comparison = a.Writes < b.Writes ? -1 : a.Writes > b.Writes ? 1 : 0;
				break;
			case 4: {
				const long double aCount = static_cast<long double>(a.Hits) + a.Writes;
				const long double bCount = static_cast<long double>(b.Hits) + b.Writes;
				if ((aCount == 0) != (bCount == 0)) return aCount != 0;
				const long double aRate = aCount == 0 ? 0 : a.Hits / aCount;
				const long double bRate = bCount == 0 ? 0 : b.Hits / bCount;
				comparison = aRate < bRate ? -1 : aRate > bRate ? 1 : 0;
				break;
			}
			}
			if (comparison == 0) return left < right;
			return sort->Direction == ImGuiSortDirection_Ascending ? comparison < 0 : comparison > 0;
		});
	}

}
