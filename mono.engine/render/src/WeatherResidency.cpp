#include "WeatherResidency.hpp"

#include "WeatherAssembly.hpp"

#include <engine/core/Profiling.hpp>
#include <engine/scene/DrawInstance.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace engine::render {
	namespace {
		constexpr uint32_t MAXIMUM_REFINEMENT_CANDIDATES = WEATHER_RESIDENCY_PAGES * 4;

		struct Domain {
			std::array<int64_t, 3> Minimum{};
			std::array<int64_t, 3> Maximum{};
		};

		using Candidate = WeatherResidencyCandidate;

		float PlanningDistance(float requested, float brickMetres) {
			const double rounded = std::ceil(static_cast<double>(requested) / brickMetres) * brickMetres;
			if (rounded >= std::numeric_limits<float>::max()) return std::numeric_limits<float>::max();
			const float distance = static_cast<float>(rounded);
			return static_cast<double>(distance) < rounded
					   ? std::nextafter(distance, std::numeric_limits<float>::infinity())
					   : distance;
		}

		int64_t FloorMultiple(int64_t coordinate, int64_t side) {
			return (coordinate / side - (coordinate < 0 && coordinate % side != 0 ? 1 : 0)) * side;
		}

		std::optional<Domain> DomainOf(const scene::WeatherCompute &controls) {
			const double cell = controls.CellSizeMetres;
			const std::array<double, 3> minimum{
				-static_cast<double>(controls.ExtentMetres.X) * 0.5,
				controls.BaseHeightMetres,
				-static_cast<double>(controls.ExtentMetres.Z) * 0.5
			};
			const std::array<double, 3> extent{
				controls.ExtentMetres.X, controls.ThicknessMetres, controls.ExtentMetres.Z
			};
			Domain domain;
			for (size_t axis = 0; axis < 3; ++axis) {
				const double low = std::floor(minimum[axis] / cell);
				const double high = std::ceil((minimum[axis] + extent[axis]) / cell);
				if (!std::isfinite(low) || !std::isfinite(high) ||
					low < std::numeric_limits<int32_t>::min() ||
					high > static_cast<double>(std::numeric_limits<int32_t>::max()) + 1.0 || high <= low)
					return std::nullopt;
				domain.Minimum[axis] = static_cast<int64_t>(low);
				domain.Maximum[axis] = static_cast<int64_t>(high);
			}
			return domain;
		}

		bool Intersects(const weather::BrickKey &key, const Domain &domain) {
			const int64_t side = static_cast<int64_t>(weather::BRICK_SIDE) << key.Lod;
			const std::array<int64_t, 3> origin{key.Origin.X, key.Origin.Y, key.Origin.Z};
			for (size_t axis = 0; axis < 3; ++axis)
				if (origin[axis] >= domain.Maximum[axis] || origin[axis] + side <= domain.Minimum[axis])
					return false;
			return true;
		}

		uint64_t InputSignature(const scene::WeatherCompute &controls, uint32_t layerKey) {
			uint64_t signature = layerKey + uint64_t{1};
			for (const float scalar :
				 {controls.CellSizeMetres,
				  controls.BaseHeightMetres,
				  controls.ThicknessMetres,
				  controls.ExtentMetres.X,
				  controls.ExtentMetres.Z,
				  controls.LodError,
				  controls.MaintenanceSeconds,
				  controls.ShadowDistanceMetres,
				  controls.WindMetresPerSecond.X,
				  controls.WindMetresPerSecond.Y,
				  controls.WindMetresPerSecond.Z,
				  controls.TurbulenceMetresPerSecond,
				  controls.ShearMetresPerSecond,
				  controls.LiftMetresPerSecond,
				  controls.GrowthRate,
				  controls.TimeScale,
				  controls.Coverage,
				  controls.VapourMixingRatio,
				  controls.VapourScale,
				  controls.CondensateScale,
				  controls.BaseTemperatureCelsius,
				  controls.TemperatureLapseCelsiusPerKilometre,
				  controls.TemperatureVariationCelsius,
				  controls.PressureVariationHectopascals,
				  controls.FrontContrast})
				signature = scene::MixSignature(signature, std::bit_cast<uint32_t>(scalar));
			for (const uint32_t scalar :
				 {controls.MaximumResidentBricks,
				  controls.MaximumLod,
				  controls.TickRate,
				  controls.Seed,
				  controls.AlgorithmVersion,
				  controls.FeatureCells,
				  static_cast<uint32_t>(controls.Paused)})
				signature = scene::MixSignature(signature, scalar);
			return signature;
		}

		std::optional<std::vector<weather::BrickKey>>
		CoarsePages(const Domain &domain, uint32_t coarseBudget, uint32_t minimumLod) {
			for (uint32_t lod = minimumLod; lod <= 27; ++lod) {
				const int64_t side = static_cast<int64_t>(weather::BRICK_SIDE) << lod;
				std::array<int64_t, 3> first{};
				std::array<uint64_t, 3> counts{};
				uint64_t count = 1;
				for (size_t axis = 0; axis < 3; ++axis) {
					first[axis] = FloorMultiple(domain.Minimum[axis], side);
					counts[axis] =
						static_cast<uint64_t>((domain.Maximum[axis] - first[axis] + side - 1) / side);
					// Clamp products before multiplication, including very large authored domains.
					if (counts[axis] > coarseBudget || count > coarseBudget / counts[axis])
						count = coarseBudget + 1;
					else
						count *= counts[axis];
				}
				if (count > coarseBudget) continue;
				std::vector<weather::BrickKey> pages;
				pages.reserve(static_cast<size_t>(count));
				for (uint64_t z = 0; z < counts[2]; ++z) {
					for (uint64_t y = 0; y < counts[1]; ++y) {
						for (uint64_t x = 0; x < counts[0]; ++x) {
							const weather::BrickKey key{
								{static_cast<int32_t>(first[0] + static_cast<int64_t>(x) * side),
								 static_cast<int32_t>(first[1] + static_cast<int64_t>(y) * side),
								 static_cast<int32_t>(first[2] + static_cast<int64_t>(z) * side)},
								lod
							};
							if (!weather::ValidBrickKey(key)) return std::nullopt;
							pages.push_back(key);
						}
					}
				}
				return pages;
			}
			return std::nullopt;
		}

		Candidate CandidateOf(
			const weather::BrickKey &key,
			float cellSize,
			std::span<const WeatherResidencyView> views,
			float motionMargin
		) {
			Candidate candidate;
			candidate.Key = key;
			candidate.DistanceMetres = std::numeric_limits<float>::infinity();
			const double side = static_cast<double>(weather::BRICK_SIDE) *
								std::ldexp(static_cast<double>(cellSize), static_cast<int>(key.Lod));
			const std::array<double, 3> centre{
				static_cast<double>(key.Origin.X) * cellSize + side * 0.5,
				static_cast<double>(key.Origin.Y) * cellSize + side * 0.5,
				static_cast<double>(key.Origin.Z) * cellSize + side * 0.5
			};
			for (const auto &view : views) {
				const auto eye = view.View.CameraPositionMetres;
				const std::array<double, 3> distance{
					std::max(0.0, std::abs(centre[0] - eye.X) - side * 0.5),
					std::max(0.0, std::abs(centre[1] - eye.Y) - side * 0.5),
					std::max(0.0, std::abs(centre[2] - eye.Z) - side * 0.5)
				};
				const double closest = std::sqrt(
					distance[0] * distance[0] + distance[1] * distance[1] + distance[2] * distance[2]
				);
				if (closest > static_cast<double>(view.MaximumDistanceMetres) + motionMargin) continue;
				candidate.DistanceMetres = std::min(candidate.DistanceMetres, static_cast<float>(closest));
				const double projected = weather::BRICK_SIDE * static_cast<double>(cellSize) *
										 view.ProjectionPixelsPerRadian /
										 std::max(static_cast<double>(cellSize), closest - motionMargin);
				candidate.ProjectedPixels = std::max(
					candidate.ProjectedPixels,
					static_cast<float>(
						std::min(projected, static_cast<double>(std::numeric_limits<float>::max()))
					)
				);
			}
			return candidate;
		}

		bool Farther(const Candidate &first, const Candidate &second) {
			if (first.DistanceMetres != second.DistanceMetres)
				return first.DistanceMetres > second.DistanceMetres;
			if (first.Key.Lod != second.Key.Lod) return first.Key.Lod > second.Key.Lod;
			if (first.Key.Origin.X != second.Key.Origin.X) return first.Key.Origin.X > second.Key.Origin.X;
			if (first.Key.Origin.Y != second.Key.Origin.Y) return first.Key.Origin.Y > second.Key.Origin.Y;
			return first.Key.Origin.Z > second.Key.Origin.Z;
		}

		float
		OpticalContrast(const weather::AssemblySummary &summary, const weather::EncodingProfile &profile) {
			const float concentrationDifference =
				static_cast<float>(summary.Scalars[0].Maximum - summary.Scalars[0].Minimum) *
				profile.CondensateKilogramsPerCubicMetre / 255.0f;
			// The represented eight samples bound their transmittance contrast over one finest cell.
			return -std::expm1(
				-weather::ExtinctionPerMetre(concentrationDifference) * profile.CellSizeMetres
			);
		}

		void BeginDemands(
			WeatherResidencyCache &cache,
			const std::vector<weather::BrickKey> &coarse,
			const Domain &domain,
			const scene::WeatherCompute &controls,
			const weather::WeatherTime &time,
			std::span<const WeatherResidencyView> views,
			float motionMargin
		) {
			auto &demands = cache.WorkDemands;
			auto &pending = cache.WorkCandidates;
			demands.clear();
			pending.clear();
			demands.reserve(MAXIMUM_REFINEMENT_CANDIDATES + coarse.size());
			pending.reserve(MAXIMUM_REFINEMENT_CANDIDATES);
			for (const auto &key : coarse) {
				const auto candidate = CandidateOf(key, controls.CellSizeMetres, views, motionMargin);
				demands.push_back({key, candidate.ProjectedPixels, 0.0f, true});
				if (std::isfinite(candidate.DistanceMetres)) pending.push_back(candidate);
			}
			std::make_heap(pending.begin(), pending.end(), Farther);
			cache.WorkMinimum = domain.Minimum;
			cache.WorkMaximum = domain.Maximum;
			cache.WorkTime = time;
			cache.WorkEvaluations = 0;
			cache.WorkCoarseCount = static_cast<uint32_t>(coarse.size());
			cache.WorkActive = true;
		}

		std::optional<uint32_t> AdvanceDemands(
			WeatherResidencyCache &cache,
			const scene::WeatherCompute &controls,
			const weather::CompiledWeather &compiled,
			std::span<const WeatherResidencyView> views,
			float motionMargin,
			uint64_t residenceTicks
		) {
			auto &demands = cache.WorkDemands;
			auto &pending = cache.WorkCandidates;
			const Domain domain{cache.WorkMinimum, cache.WorkMaximum};
			weather::LodPolicy lodPolicy;
			lodPolicy.MaximumLod = std::min(controls.MaximumLod, uint32_t{27});
			lodPolicy.MaximumOpticalError = controls.LodError;
			lodPolicy.MinimumResidenceTicks = controls.TickRate;
			uint32_t evaluations = 0;
			while (!pending.empty() && evaluations < WEATHER_MAINTENANCE_CHUNK &&
				   cache.WorkEvaluations < MAXIMUM_REFINEMENT_CANDIDATES) {
				std::pop_heap(pending.begin(), pending.end(), Farther);
				const Candidate candidate = pending.back();
				pending.pop_back();
				++evaluations;
				++cache.WorkEvaluations;
				if (candidate.Key.Lod == 0 || candidate.ProjectedPixels <= 0.0f) continue;
				const auto empty = WeatherPageIsEmpty(compiled, candidate.Key);
				if (!empty) return std::nullopt;
				if (*empty) continue;
				const auto summary = WeatherPageAssembly(compiled, cache.WorkTime, candidate.Key);
				if (!summary) return std::nullopt;
				const float opticalError = OpticalContrast(*summary, compiled.Profile);
				const auto desiredLod = weather::ChooseLod(
					candidate.ProjectedPixels,
					opticalError,
					std::min(candidate.Key.Lod, lodPolicy.MaximumLod),
					residenceTicks,
					lodPolicy
				);
				const bool preservesSamples = weather::CanMerge(*summary, compiled.Profile);
				if (desiredLod && *desiredLod >= candidate.Key.Lod && preservesSamples) continue;
				const uint32_t childLod = candidate.Key.Lod - 1;
				const int64_t childSide = static_cast<int64_t>(weather::BRICK_SIDE) << childLod;
				for (uint32_t child = 0;
					 child < 8 && demands.size() < MAXIMUM_REFINEMENT_CANDIDATES + cache.WorkCoarseCount;
					 ++child) {
					const weather::BrickKey key{
						{static_cast<int32_t>(candidate.Key.Origin.X + ((child & 1) ? childSide : 0)),
						 static_cast<int32_t>(candidate.Key.Origin.Y + ((child & 2) ? childSide : 0)),
						 static_cast<int32_t>(candidate.Key.Origin.Z + ((child & 4) ? childSide : 0))},
						childLod
					};
					if (!weather::ValidBrickKey(key) || !Intersects(key, domain)) continue;
					const auto refined = CandidateOf(key, controls.CellSizeMetres, views, motionMargin);
					if (!std::isfinite(refined.DistanceMetres)) continue;
					demands.push_back({key, refined.ProjectedPixels, opticalError, false});
					if (pending.size() < MAXIMUM_REFINEMENT_CANDIDATES) {
						pending.push_back(refined);
						std::push_heap(pending.begin(), pending.end(), Farther);
					}
				}
			}
			cache.WorkActive = !pending.empty() && cache.WorkEvaluations < MAXIMUM_REFINEMENT_CANDIDATES;
			return evaluations;
		}
		bool PublishSelection(
			WeatherResidencyCache &cache,
			const weather::ResidencySelection &selection,
			const weather::CompiledWeather &compiled,
			uint64_t tick,
			uint32_t tickRate,
			double presentationSeconds
		) {
			const uint64_t previousSignature = cache.GeometrySignature;
			cache.Selected = selection.Pages;
			std::sort(
				cache.Selected.begin(), cache.Selected.end(), [](const auto &first, const auto &second) {
					if (first.Lod != second.Lod) return first.Lod < second.Lod;
					if (first.Origin.X != second.Origin.X) return first.Origin.X < second.Origin.X;
					if (first.Origin.Y != second.Origin.Y) return first.Origin.Y < second.Origin.Y;
					return first.Origin.Z < second.Origin.Z;
				}
			);
			uint64_t topology = 1;
			for (const auto &key : cache.Selected)
				for (const uint32_t lane :
					 {static_cast<uint32_t>(key.Origin.X),
					  static_cast<uint32_t>(key.Origin.Y),
					  static_cast<uint32_t>(key.Origin.Z),
					  key.Lod})
					topology = scene::MixSignature(topology, lane);
			if (topology != previousSignature && !cache.PageKeys.empty()) {
				cache.PreviousPageKeys = cache.PageKeys;
				cache.TransitionStartTick = tick;
				cache.TransitionDurationTicks = std::max(uint32_t{1}, tickRate / 2);
				cache.TransitionStartSeconds = presentationSeconds;
				cache.TransitionUsesPresentationClock = presentationSeconds >= 0.0;
			}
			cache.PageKeys.clear();
			cache.PageKeys.reserve(WEATHER_RESIDENCY_PAGES);
			for (const auto &key : cache.Selected)
				cache.PageKeys.emplace_back(
					key.Origin.X, key.Origin.Y, key.Origin.Z, static_cast<int32_t>(key.Lod)
				);
			cache.GeometrySignature = topology;
			cache.PageKinds.assign(cache.PageKeys.size(), 0);
			cache.FieldWords = 0;
			for (size_t index = 0; index < cache.Selected.size(); ++index) {
				const auto empty = WeatherPageIsEmpty(compiled, cache.Selected[index]);
				if (!empty) return false;
				cache.PageKinds[index] = *empty ? 2 : 0;
				if (!*empty) cache.FieldWords += weather::BRICK_CELLS * 2;
			}
			for (const uint8_t kind : cache.PageKinds)
				topology = scene::MixSignature(topology, kind);
			cache.TopologySignature = topology;
			cache.CoarsePages = cache.WorkCoarseCount - selection.CoarseFallbackRegions;
			cache.RefusedRefinements = selection.RefusedRefinements;
			cache.RequiresProceduralFallback = selection.RequiresProceduralFallback;
			return true;
		}

		uint64_t NextMaintenance(uint64_t tick, const scene::WeatherCompute &controls, uint32_t layerKey) {
			const uint64_t period = std::max(
				uint64_t{1},
				static_cast<uint64_t>(
					std::ceil(static_cast<double>(controls.MaintenanceSeconds) * controls.TickRate)
				)
			);
			const uint64_t phase = weather::Hash(layerKey ^ weather::Hash(controls.Seed)) % period;
			uint64_t delay = period - tick % period + phase;
			if (delay > period) delay -= period;
			return tick > std::numeric_limits<uint64_t>::max() - delay ? std::numeric_limits<uint64_t>::max()
																	   : tick + delay;
		}

	}

	std::optional<weather::AssemblySummary> WeatherPageAssembly(
		const weather::CompiledWeather &compiled,
		const weather::WeatherTime &time,
		const weather::BrickKey &page
	) {
		if (!weather::ValidBrickKey(page)) return std::nullopt;
		const weather::AssemblyRegion region{page.Origin, uint32_t{4} << page.Lod, page.Lod};
		const auto children = weather::EvaluateChildren(compiled.Definition, time, region);
		if (!children) return std::nullopt;
		return weather::Summarize(region, time, *children);
	}

	uint64_t WeatherResidencyMetadataBytes(const WeatherResidencyCache &cache) {
		return sizeof(cache) + cache.Maintenance.Views.capacity() * sizeof(weather::ResidencyView) +
			   cache.Selected.capacity() * sizeof(weather::BrickKey) +
			   (cache.PageKeys.capacity() + cache.PreviousPageKeys.capacity()) * sizeof(glm::ivec4) +
			   cache.WorkDemands.capacity() * sizeof(weather::BrickDemand) +
			   cache.WorkCandidates.capacity() * sizeof(WeatherResidencyCandidate) +
			   cache.PageKinds.capacity();
	}

	std::optional<WeatherResidencyPlan> BuildWeatherResidency(
		WeatherResidencyCache &cache,
		const scene::WeatherCompute &controls,
		const weather::CompiledWeather &compiled,
		const weather::WeatherTime &time,
		uint64_t timelineRevision,
		std::span<const WeatherResidencyView> views,
		uint64_t frameSequence,
		double presentationSeconds
	) {
		ENGINE_PROFILE_CAT("weather sparse residency plan", core::ProfileCategory::Render);
		if (!scene::ValidWeatherCompute(controls) || controls.Mode != scene::WeatherMode::Procedural ||
			time.Revision != compiled.Definition.Revision || views.size() > weather::MAXIMUM_RESIDENCY_VIEWS)
			return std::nullopt;
		if (presentationSeconds != -1.0 && (!std::isfinite(presentationSeconds) || presentationSeconds < 0.0))
			return std::nullopt;
		std::array<weather::ResidencyView, weather::MAXIMUM_RESIDENCY_VIEWS> viewPositions{};
		std::array<WeatherResidencyView, weather::MAXIMUM_RESIDENCY_VIEWS> planningViews{};
		for (size_t index = 0; index < views.size(); ++index) {
			if (!std::isfinite(views[index].ProjectionPixelsPerRadian) ||
				views[index].ProjectionPixelsPerRadian <= 0.0f ||
				!std::isfinite(views[index].MaximumDistanceMetres) ||
				views[index].MaximumDistanceMetres < 0.0f)
				return std::nullopt;
			viewPositions[index] = views[index].View;
			planningViews[index] = views[index];
			planningViews[index].MaximumDistanceMetres = PlanningDistance(
				views[index].MaximumDistanceMetres, weather::BRICK_SIDE * controls.CellSizeMetres
			);
		}
		// Both the retained signature and candidate coverage use the expanded range, so small sun
		// movements can finish maintenance without losing newly requested shadow casters.
		views = std::span(planningViews).first(views.size());
		const auto positions = std::span(viewPositions).first(views.size());
		weather::MaintenancePolicy maintenancePolicy;
		maintenancePolicy.TickRate = controls.TickRate;
		maintenancePolicy.IntervalSeconds = controls.MaintenanceSeconds;
		maintenancePolicy.MaximumCameraTravelMetres = weather::BRICK_SIDE * controls.CellSizeMetres * 0.5f;
		uint64_t inputSignature = InputSignature(controls, compiled.Definition.LayerKey);
		std::array<std::pair<uint64_t, uint64_t>, weather::MAXIMUM_RESIDENCY_VIEWS> viewSignatures{};
		for (size_t index = 0; index < views.size(); ++index) {
			uint64_t signature =
				scene::MixSignature(1, std::bit_cast<uint32_t>(views[index].ProjectionPixelsPerRadian));
			signature =
				scene::MixSignature(signature, std::bit_cast<uint32_t>(views[index].MaximumDistanceMetres));
			viewSignatures[index] = {views[index].View.Key, signature};
		}
		std::sort(viewSignatures.begin(), viewSignatures.begin() + static_cast<ptrdiff_t>(views.size()));
		for (size_t index = 0; index < views.size(); ++index) {
			inputSignature = scene::MixSignature(inputSignature, viewSignatures[index].first);
			inputSignature = scene::MixSignature(inputSignature, viewSignatures[index].second);
		}
		// Cadence is phase-staggered by the stable layer key. This check handles immediate invalidation.
		auto invalidationPolicy = maintenancePolicy;
		invalidationPolicy.IntervalSeconds = std::numeric_limits<float>::max();
		const bool transitionInProgress =
			!cache.PreviousPageKeys.empty() &&
			(cache.TransitionUsesPresentationClock
				 ? presentationSeconds >= cache.TransitionStartSeconds &&
					   presentationSeconds - cache.TransitionStartSeconds < WEATHER_LOD_TRANSITION_SECONDS
				 : time.Tick >= cache.TransitionStartTick &&
					   time.Tick - cache.TransitionStartTick < cache.TransitionDurationTicks);
		const bool rebuild =
			inputSignature != cache.InputSignature ||
			weather::MaintenanceRequired(
				cache.Maintenance, positions, time.Tick, time.Revision, timelineRevision, invalidationPolicy
			) ||
			(!cache.WorkActive && !cache.ClassificationActive && !transitionInProgress &&
			 time.Tick >= cache.NextMaintenanceTick);
		const uint64_t age = cache.Maintenance.Valid && time.Tick >= cache.Maintenance.Tick
								 ? time.Tick - cache.Maintenance.Tick
								 : 0;
		weather::MotionEnvelope envelope;
		const auto &definition = compiled.Definition;
		const auto scales = compiled.Profile.WindScaleMetresPerSecond;
		const std::array<float, 3> windScales{scales.X, scales.Y, scales.Z};
		std::array<float, 3> componentBounds{};
		for (size_t axis = 0; axis < componentBounds.size(); ++axis) {
			const int64_t shapedCode = axis == 1 ? std::abs(static_cast<int64_t>(definition.Lift))
												 : std::abs(static_cast<int64_t>(definition.Shear));
			const int64_t maximumCode = std::abs(static_cast<int64_t>(definition.WindCodes[axis])) +
										definition.Turbulence + shapedCode;
			componentBounds[axis] =
				static_cast<float>(std::min(maximumCode, int64_t{127})) * windScales[axis] / 127;
		}
		// Turbulence and shear act on several axes. A scalar amplitude is not a speed bound.
		const core::Vector3 maximumWind{componentBounds[0], componentBounds[1], componentBounds[2]};
		envelope.MaximumSpeedMetresPerSecond =
			std::max(controls.WindMetresPerSecond.Magnitude(), maximumWind.Magnitude());
		envelope.GrowthMetresPerSecond = controls.GrowthRate * controls.CellSizeMetres;
		const float progression = controls.Paused ? 0.0f : controls.TimeScale;
		envelope.MaximumSpeedMetresPerSecond *= progression;
		envelope.GrowthMetresPerSecond *= progression;
		envelope.ReconstructionMarginMetres = maintenancePolicy.MaximumCameraTravelMetres;
		const float lifetime = std::max(
			controls.MaintenanceSeconds, static_cast<float>(static_cast<double>(age) / controls.TickRate)
		);
		const auto margin = weather::MotionMargin(envelope, lifetime);
		if (!margin) return std::nullopt;
		bool published = false;
		uint32_t candidateEvaluations = 0;
		uint32_t classifiedPages = 0;
		const uint32_t pageBudget = std::min(controls.MaximumResidentBricks, WEATHER_RESIDENCY_PAGES);
		if (rebuild) {
			auto newStamp = weather::RecordMaintenance(positions, time.Tick, time.Revision, timelineRevision);
			if (!newStamp) return std::nullopt;
			const auto domain = DomainOf(controls);
			if (!domain) return std::nullopt;
			const uint32_t coarseBudget =
				std::min(WEATHER_COARSE_PAGES, std::max(uint32_t{8}, pageBudget / 4));
			const auto coarse =
				CoarsePages(*domain, coarseBudget, std::min(controls.MaximumLod, uint32_t{27}));
			if (!coarse) return std::nullopt;
			BeginDemands(cache, *coarse, *domain, controls, time, views, *margin);
			const auto coverage = weather::SelectResidency(cache.WorkDemands, pageBudget);
			if (!coverage) return std::nullopt;
			if (!PublishSelection(
					cache, *coverage, compiled, time.Tick, controls.TickRate, presentationSeconds
				))
				return std::nullopt;
			cache.InputSignature = inputSignature;
			cache.Maintenance = std::move(*newStamp);
			cache.NextMaintenanceTick = NextMaintenance(time.Tick, controls, compiled.Definition.LayerKey);
			cache.ClassificationActive = false;
			published = true;
		}
		// Frame identity keeps paused weather progressing without multiplying work for each viewport.
		const uint64_t advanceSequence = frameSequence != 0 ? frameSequence : time.Tick;
		const bool advance = rebuild || cache.LastAdvanceSequence != advanceSequence;
		if (cache.WorkActive && advance) {
			const auto advanced = AdvanceDemands(
				cache,
				controls,
				compiled,
				views,
				*margin,
				std::max(age, static_cast<uint64_t>(controls.TickRate))
			);
			if (!advanced) return std::nullopt;
			candidateEvaluations = *advanced;
			cache.LastAdvanceSequence = advanceSequence;
			if (!cache.WorkActive) {
				const auto completed = weather::SelectResidency(cache.WorkDemands, pageBudget);
				if (!completed) return std::nullopt;
				if (!PublishSelection(
						cache, *completed, compiled, time.Tick, controls.TickRate, presentationSeconds
					))
					return std::nullopt;
				cache.ClassificationIndex = 0;
				cache.ClassificationActive = true;
				published = true;
				cache.WorkDemands.clear();
				cache.WorkCandidates.clear();
				if (cache.NextMaintenanceTick <= time.Tick)
					cache.NextMaintenanceTick =
						NextMaintenance(time.Tick, controls, compiled.Definition.LayerKey);
			}
		}
		if (cache.ClassificationActive && advance) {
			while (cache.ClassificationIndex < cache.Selected.size() &&
				   classifiedPages < WEATHER_CLASSIFICATION_CHUNK) {
				const uint32_t index = cache.ClassificationIndex++;
				if (cache.PageKinds[index] == 2 || cache.Selected[index].Lod == 0) continue;
				const auto compact = WeatherPageCanCompress(compiled, cache.WorkTime, cache.Selected[index]);
				if (!compact) return std::nullopt;
				cache.PageKinds[index] = *compact ? 1 : 0;
				++classifiedPages;
			}
			cache.LastAdvanceSequence = advanceSequence;
			if (cache.ClassificationIndex == cache.Selected.size()) {
				cache.ClassificationActive = false;
				cache.FieldWords = 0;
				uint64_t signature = cache.GeometrySignature;
				for (const uint8_t kind : cache.PageKinds) {
					cache.FieldWords += kind == 2 ? 0
												  : (kind == 1 ? WEATHER_COMPRESSED_PAGE_BYTES / 4
															   : weather::BRICK_PAYLOAD_BYTES / 4);
					signature = scene::MixSignature(signature, kind);
				}
				cache.TopologySignature = signature;
				published = true;
			}
		}
		float transitionAlpha = 1.0f;
		if (!cache.PreviousPageKeys.empty() && cache.TransitionDurationTicks != 0) {
			const uint64_t elapsed =
				time.Tick >= cache.TransitionStartTick ? time.Tick - cache.TransitionStartTick : 0;
			transitionAlpha =
				static_cast<float>(std::min(elapsed, static_cast<uint64_t>(cache.TransitionDurationTicks))) /
				static_cast<float>(cache.TransitionDurationTicks);
			if (cache.TransitionUsesPresentationClock) {
				const double seconds = presentationSeconds >= cache.TransitionStartSeconds
										   ? presentationSeconds - cache.TransitionStartSeconds
										   : WEATHER_LOD_TRANSITION_SECONDS;
				transitionAlpha = static_cast<float>(std::min(seconds / WEATHER_LOD_TRANSITION_SECONDS, 1.0));
			}
			if (transitionAlpha >= 1.0f) cache.PreviousPageKeys.clear();
		}
		const uint64_t maintenanceAge =
			time.Tick >= cache.Maintenance.Tick ? time.Tick - cache.Maintenance.Tick : 0;
		return WeatherResidencyPlan{
			cache.PageKeys,
			cache.TopologySignature,
			maintenanceAge,
			cache.CoarsePages,
			cache.RefusedRefinements,
			*margin,
			published,
			cache.RequiresProceduralFallback,
			cache.PreviousPageKeys,
			transitionAlpha,
			candidateEvaluations,
			cache.WorkActive || cache.ClassificationActive,
			WeatherResidencyMetadataBytes(cache),
			cache.PageKinds,
			cache.FieldWords,
			classifiedPages
		};
	}

	std::optional<weather::PageGeneration> ReserveWeatherPages(
		weather::ResidencyState &state,
		const WeatherResidencyCache &cache,
		uint64_t tick,
		uint32_t revision,
		uint64_t completedSubmission
	) {
		return weather::ReserveGeneration(state, cache.Selected, tick, revision, completedSubmission);
	}
}
