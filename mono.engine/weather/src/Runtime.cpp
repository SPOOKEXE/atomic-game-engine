#include <engine/core/Bytes.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/ecs/Components.hpp>
#include <engine/ecs/Instance.hpp>
#include <engine/ecs/Scheduler.hpp>
#include <engine/ecs/Store.hpp>
#include <engine/scene/Characters.hpp>
#include <engine/scene/Components.hpp>
#include <engine/scene/Controls.hpp>
#include <engine/weather/Clock.hpp>
#include <engine/weather/Runtime.hpp>
#include <engine/weather/StatefulRuntime.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

namespace engine::weather {
	namespace {
		constexpr uint32_t Q16_ONE = 65536;
		constexpr uint64_t MAX_CATCH_UP_TICKS = 32;
		// Scene bounds permit 16 weather seconds/base tick and ten discharges/weather second.
		constexpr uint32_t MAX_LIGHTNING_EVENTS_PER_TICK = 160;
		constexpr uint32_t MAX_HAIL_EVENTS_PER_TICK = 16;
		constexpr float LIGHTNING_DAMAGE = 50.0f;
		constexpr float HAIL_DAMAGE = 5.0f;
		constexpr float LIGHTNING_RADIUS_METRES = 8.0f;
		constexpr float HAIL_RADIUS_METRES = 4.0f;
		constexpr float MAX_GROUND_QUERY_DISTANCE_METRES = 100000.0f;
		std::string DesiredLayerName(const ecs::Store &store, ecs::Entity provider) {
			std::string layer;
			if (const auto *controls = store.Get<scene::WeatherCompute>(provider)) {
				const auto purpose = controls->LayerName.Text();
				if (purpose.empty() || purpose.size() > 128) return {};
				layer = std::to_string(purpose.size()) + ":" + std::string(purpose);
			}
			for (uint32_t depth = 0; provider != ecs::NULL_ENTITY && depth < 64; ++depth) {
				const auto instanceName = store.InstanceNameOf(provider);
				const auto name = instanceName.IsValid() ? instanceName.Text() : store.NameOf(provider);
				if (name.empty()) return {};
				const std::string prefix = std::to_string(name.size()) + ":" + std::string(name) + "/";
				if (prefix.size() + layer.size() > MAX_WEATHER_LAYER_NAME_BYTES) return {};
				layer.insert(0, prefix);
				provider = store.ParentOf(provider);
			}
			return provider == ecs::NULL_ENTITY ? layer : std::string{};
		}
		bool InvalidPhysicalHistory(const ecs::Store &store, ecs::Entity provider) {
			const auto *accepted = store.Get<scene::WeatherAcceptedDefinition>(provider);
			if (!accepted || accepted->Controls.Mode != scene::WeatherMode::Stateful) return false;
			if (store.Has<StatefulWeatherInitializationFailure>(provider)) return true;
			if (const auto *physical = store.Get<StatefulWeatherState>(provider))
				return physical->LastStatus != SimulationStatus::Success;
			auto normalized = accepted->Controls;
			normalized.TimeScale = 1;
			normalized.Paused = false;
			return !MakeSimulationDefinition(normalized, WeatherLayerName(store, provider));
		}

		void CompileLayer(
			ecs::Store &store,
			WeatherRuntimeCache &cache,
			ecs::Entity provider,
			const scene::WeatherCompute &controls,
			bool forceCompilation = true,
			bool acceptDesiredEdits = false
		) {
			auto found = std::find_if(cache.Layers.begin(), cache.Layers.end(), [&](const auto &layer) {
				return layer.Provider == provider;
			});
			if (found == cache.Layers.end()) {
				if (cache.Layers.size() >= MAX_RUNTIME_WEATHER_LAYERS) return;
				cache.Layers.push_back({provider, {}, WeatherLayerName(store, provider)});
				found = std::prev(cache.Layers.end());
			}
			const auto *accepted = store.Get<scene::WeatherAcceptedDefinition>(provider);
			const auto *clock = store.Get<scene::WeatherClock>(provider);
			const auto *runtime = store.Get<WeatherRuntimeState>(provider);
			const bool backlog = clock && (!runtime || !runtime->Ready || runtime->LastTick < clock->Tick) &&
								 !InvalidPhysicalHistory(store, provider);
			const bool newClockBoundary = !clock || clock->SimulationTick < store.Time().Tick;
			const auto desiredLayer = DesiredLayerName(store, provider);
			const bool defer =
				backlog || !acceptDesiredEdits || !newClockBoundary || !scene::ValidWeatherCompute(controls);
			const bool pendingControls =
				!store.AdoptOnly() && accepted && accepted->Controls != controls && defer;
			const bool pendingIdentity = !store.AdoptOnly() && accepted && !accepted->Layer.empty() &&
										 accepted->Layer != desiredLayer && defer &&
										 !found->AmbiguousIdentity;
			found->PendingDefinition = pendingControls || pendingIdentity;
			auto chosen = (store.AdoptOnly() || pendingControls) && accepted ? accepted->Controls : controls;
			auto chosenLayer = (store.AdoptOnly() || pendingIdentity) && accepted && !accepted->Layer.empty()
								   ? accepted->Layer
								   : desiredLayer;
			if (!store.AdoptOnly() && scene::ValidWeatherCompute(chosen)) {
				bool admitted = false;
				if (clock && accepted && !accepted->Layer.empty() && chosenLayer != accepted->Layer) {
					const auto epoch = ObserveWeatherEpoch(store, accepted->Layer, clock->Epoch)
										   ? ClaimWeatherEpoch(store, chosenLayer, clock->Epoch - 1)
										   : std::nullopt;
					admitted = epoch.has_value();
					if (epoch && *epoch != clock->Epoch) {
						const auto resolved = WeatherTimeOf(*clock);
						if (!resolved) return;
						auto renamed = *clock;
						renamed.Epoch = *epoch;
						renamed.AnchorTick = renamed.Tick;
						renamed.PhaseTick = resolved->PhaseTick;
						renamed.PhaseFractionQ16 = resolved->PhaseFractionQ16;
						renamed.DisplacementQ8 = resolved->DisplacementQ8;
						renamed.DisplacementRemainderQ12 = resolved->DisplacementRemainderQ12;
						store.Set(provider, renamed);
					}
				} else
					admitted = ObserveWeatherEpoch(store, chosenLayer, clock ? clock->Epoch : 0);
				if (!admitted) {
					found->PendingDefinition = true;
					if (!accepted || accepted->Layer.empty()) {
						found->Compiled.reset();
						return;
					}
					chosen = accepted->Controls;
					chosenLayer = accepted->Layer;
				}
			}
			const bool acceptedChanged =
				!accepted || accepted->Controls != chosen || accepted->Layer != chosenLayer;
			if (!store.AdoptOnly() && acceptedChanged && scene::ValidWeatherCompute(chosen))
				store.Set(provider, scene::WeatherAcceptedDefinition{chosen, chosenLayer});
			const auto &layerName = chosenLayer;
			if (found->PendingDefinition && found->Layer == layerName && found->Compiled) return;
			if (!forceCompilation && found->Layer == layerName && found->Compiled) return;
			found->Layer = layerName;
			// Stateful samples come from solver cells; this prepares only geometry and timeline arithmetic.
			auto referenceControls = chosen;
			referenceControls.Mode = scene::WeatherMode::Procedural;
			found->Compiled = found->Layer.empty() ? std::nullopt : Compile(referenceControls);
			if (cache.Compilations != UINT64_MAX) ++cache.Compilations;
			core::Metrics::Count("weather.definition.compilations", 1.0);
		}
		void ValidateLayerIdentities(ecs::Store &store, WeatherRuntimeCache &cache) {
			// A rejected duplicate has no valid old stream to drain. Renaming it must permit recovery,
			// including after load resets the derived ambiguity flag, while preserving event high-water
			// marks.
			for (auto &layer : cache.Layers) {
				if (!layer.PendingDefinition ||
					std::count_if(cache.Layers.begin(), cache.Layers.end(), [&](const auto &candidate) {
						return candidate.Layer == layer.Layer;
					}) < 2)
					continue;
				const auto desired = DesiredLayerName(store, layer.Provider);
				if (desired.empty() || desired == layer.Layer ||
					std::count_if(cache.Layers.begin(), cache.Layers.end(), [&](const auto &candidate) {
						return DesiredLayerName(store, candidate.Provider) == desired;
					}) != 1)
					continue;
				layer.AmbiguousIdentity = true;
				if (const auto *controls = store.Get<scene::WeatherCompute>(layer.Provider))
					CompileLayer(store, cache, layer.Provider, *controls);
			}
			for (auto &layer : cache.Layers) {
				const bool ambiguous =
					!layer.Layer.empty() &&
					std::count_if(cache.Layers.begin(), cache.Layers.end(), [&](const auto &candidate) {
						return candidate.Layer == layer.Layer;
					}) > 1;
				if (ambiguous) {
					layer.Compiled.reset();
					store.Remove<WeatherEvents>(layer.Provider);
				} else if (layer.AmbiguousIdentity) {
					if (const auto *controls = store.Get<scene::WeatherCompute>(layer.Provider))
						CompileLayer(store, cache, layer.Provider, *controls);
				}
				layer.AmbiguousIdentity = ambiguous;
				if (const auto *events = store.Get<WeatherEvents>(layer.Provider);
					events && !events->Events.empty() && events->Events.back().Layer != layer.Layer)
					store.Remove<WeatherEvents>(layer.Provider);
			}
		}
		void WriteRuntime(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *states = static_cast<const WeatherRuntimeState *>(source);
			for (size_t index = 0; index < count; ++index) {
				const auto &state = states[index];
				writer.WriteUInt32(1);
				writer.WriteUInt64(state.Epoch);
				writer.WriteUInt64(state.LastTick);
				writer.WriteUInt64(state.NextSequence);
				writer.WriteUInt32(state.Seed);
				writer.WriteUInt32(state.ChargeQ16);
				writer.WriteUInt32(state.HailGrowthQ16);
				writer.WriteBool(state.Ready);
			}
		}
		void ReadRuntime(core::ByteReader &reader, void *destination, size_t count) {
			auto *states = static_cast<WeatherRuntimeState *>(destination);
			for (size_t index = 0; index < count; ++index) {
				const uint32_t version = reader.ReadUInt32();
				WeatherRuntimeState decoded;
				decoded.Epoch = reader.ReadUInt64();
				decoded.LastTick = reader.ReadUInt64();
				decoded.NextSequence = reader.ReadUInt64();
				decoded.Seed = reader.ReadUInt32();
				decoded.ChargeQ16 = reader.ReadUInt32();
				decoded.HailGrowthQ16 = reader.ReadUInt32();
				const uint8_t ready = reader.ReadUInt8();
				decoded.Ready = ready != 0;
				if (reader.Failed() || version != 1 || decoded.Epoch == 0 || decoded.NextSequence == 0 ||
					decoded.ChargeQ16 > Q16_ONE || decoded.HailGrowthQ16 > Q16_ONE || ready > 1) {
					reader.Fail();
					return;
				}
				states[index] = decoded;
			}
		}
		void WriteEvents(core::ByteWriter &writer, const void *source, size_t count) {
			const auto *journals = static_cast<const WeatherEvents *>(source);
			for (size_t index = 0; index < count; ++index) {
				writer.WriteUInt32(1);
				writer.WriteUInt64(journals[index].Dropped);
				writer.WriteUInt32(static_cast<uint32_t>(journals[index].Events.size()));
				for (const auto &event : journals[index].Events)
					WriteStormEvent(writer, event);
			}
		}
		void ReadEvents(core::ByteReader &reader, void *destination, size_t count) {
			auto *journals = static_cast<WeatherEvents *>(destination);
			for (size_t index = 0; index < count; ++index) {
				const uint32_t version = reader.ReadUInt32();
				WeatherEvents decoded;
				decoded.Dropped = reader.ReadUInt64();
				const uint32_t eventCount = reader.ReadUInt32();
				if (version != 1 || eventCount > MAX_WEATHER_EVENTS) {
					reader.Fail();
					return;
				}
				for (uint32_t eventIndex = 0; eventIndex < eventCount; ++eventIndex) {
					StormEvent event;
					if (!ReadStormEvent(reader, event) || !AppendWeatherEvent(decoded, event)) {
						reader.Fail();
						return;
					}
				}
				if (reader.Failed()) return;
				journals[index] = std::move(decoded);
			}
		}
		template <class T> void WriteLocal(core::ByteWriter &, const void *, size_t) {}
		template <class T> void ReadLocal(core::ByteReader &, void *destination, size_t count) {
			auto *records = static_cast<T *>(destination);
			for (size_t index = 0; index < count; ++index)
				records[index] = {};
		}
		bool Emit(
			ecs::Store &store,
			WeatherRuntimeState &state,
			WeatherEvents &events,
			const PreparedWeatherLayer &layer,
			const scene::WeatherCompute &controls,
			StormEventKind kind,
			uint64_t tick,
			core::Vector3 ground
		) {
			if (state.NextSequence == UINT64_MAX) return false;
			const auto *groundQuery = store.Resource<WeatherGroundQuery>();
			if (!groundQuery || !groundQuery->Height) return false;
			StormEvent event;
			event.Layer = layer.Layer;
			event.Id = {state.Epoch, state.NextSequence, state.Seed};
			event.Kind = kind;
			event.Tick = tick;
			event.TickRate = controls.TickRate;
			const auto *clock = store.Get<scene::WeatherClock>(layer.Provider);
			const auto elapsed = clock ? ElapsedAtTick(*clock, tick) : std::nullopt;
			if (!elapsed) return false;
			event.ElapsedSeconds = elapsed->Seconds;
			event.ElapsedFractionQ32 = elapsed->FractionQ32;
			event.EndMetres = ground;
			event.StartMetres = {
				ground.X, controls.BaseHeightMetres + controls.ThicknessMetres * 0.75f, ground.Z
			};
			// The source cell and seed stay canonical. Only the authority resolves the copied impact
			// endpoint.
			const auto height =
				groundQuery->Height(store, event.StartMetres, MAX_GROUND_QUERY_DISTANCE_METRES);
			if (!height || !std::isfinite(*height) || *height > event.StartMetres.Y ||
				event.StartMetres.Y - *height > MAX_GROUND_QUERY_DISTANCE_METRES)
				return false;
			event.EndMetres.Y = *height;
			event.BranchSeed = Hash(
				layer.Compiled->Definition.LayerKey ^ state.Seed ^
				Hash(static_cast<uint32_t>(state.NextSequence)) ^
				Hash(static_cast<uint32_t>(state.NextSequence >> 32u))
			);
			event.Damage = kind == StormEventKind::Lightning ? LIGHTNING_DAMAGE : HAIL_DAMAGE;
			event.RadiusMetres =
				kind == StormEventKind::Lightning ? LIGHTNING_RADIUS_METRES : HAIL_RADIUS_METRES;
			if (!AppendWeatherEvent(events, event)) return false;
			WeatherEventCursor once;
			if (!ApplyStormDamage(store, once, event)) return false;
			++state.NextSequence;
			core::Metrics::Count("weather.authoritative.events", 1.0);
			return true;
		}
	}

	std::string WeatherLayerName(const ecs::Store &store, ecs::Entity provider) {
		if (const auto *accepted = store.Get<scene::WeatherAcceptedDefinition>(provider);
			accepted && !accepted->Layer.empty())
			return accepted->Layer;
		return DesiredLayerName(store, provider);
	}

	void RegisterWeatherRuntimeComponents() {
		RegisterWeatherEpochHistory();
		RegisterStatefulWeatherComponents();
		ecs::Components::Register<WeatherRuntimeState>("weather.RuntimeState", WriteRuntime, ReadRuntime, 41);
		ecs::Components::Register<WeatherEvents>("weather.Events", WriteEvents, ReadEvents, 12000);
		ecs::Components::Register<WeatherRuntimeCache>(
			"weather.RuntimeCache", WriteLocal<WeatherRuntimeCache>, ReadLocal<WeatherRuntimeCache>
		);
		ecs::Components::Register<WeatherListener>(
			"weather.Listener", WriteLocal<WeatherListener>, ReadLocal<WeatherListener>
		);
		ecs::Components::Register<WeatherGroundQuery>(
			"weather.GroundQuery", WriteLocal<WeatherGroundQuery>, ReadLocal<WeatherGroundQuery>
		);
	}
	void PrepareWeatherRuntime(ecs::Store &store, bool acceptDesiredEdits) {
		store.Observe<scene::WeatherCompute>();
		store.Observe<scene::WeatherAcceptedDefinition>();
		store.Observe<ecs::InstanceName>();
		store.Observe<ecs::Hierarchy>();
		if (!store.HasResource<WeatherRuntimeCache>()) store.SetResource(WeatherRuntimeCache{});
		auto &cache = *store.ResourceMutable<WeatherRuntimeCache>();
		const auto writes = store.ComponentChangeVersion<scene::WeatherCompute>();
		const auto membership = store.ComponentMembershipVersion<scene::WeatherCompute>();
		const auto names = store.ComponentChangeVersion<ecs::InstanceName>();
		const auto hierarchy = store.ComponentChangeVersion<ecs::Hierarchy>();
		const auto acceptedWrites = store.ComponentChangeVersion<scene::WeatherAcceptedDefinition>();
		const auto acceptedMembership = store.ComponentMembershipVersion<scene::WeatherAcceptedDefinition>();
		const bool acceptedChanges =
			acceptedWrites != cache.AcceptedWrites || acceptedMembership != cache.AcceptedMembership ||
			cache.AcceptedWrites < store.ComponentClearedChangeVersion<scene::WeatherAcceptedDefinition>();
		const bool definitionChanges =
			!cache.Ready || writes != cache.DefinitionWrites || membership != cache.DefinitionMembership ||
			cache.DefinitionWrites < store.ComponentClearedChangeVersion<scene::WeatherCompute>() ||
			acceptedChanges;
		const bool full =
			!cache.Ready || membership != cache.DefinitionMembership ||
			cache.DefinitionWrites < store.ComponentClearedChangeVersion<scene::WeatherCompute>() ||
			names != cache.NameWrites || hierarchy != cache.HierarchyWrites || acceptedChanges;
		if (full) {
			ENGINE_PROFILE("weather definition membership");
			std::erase_if(cache.Layers, [&](const auto &layer) {
				return !store.Has<scene::WeatherCompute>(layer.Provider);
			});
			store.Each<const WeatherRuntimeState>([&](ecs::Entity provider, const auto &) {
				if (!store.Has<scene::WeatherCompute>(provider)) store.Remove<WeatherRuntimeState>(provider);
			});
			store.Each<const WeatherEvents>([&](ecs::Entity provider, const auto &) {
				if (!store.Has<scene::WeatherCompute>(provider)) store.Remove<WeatherEvents>(provider);
			});
			store.Each<const scene::WeatherAcceptedDefinition>([&](ecs::Entity provider, const auto &) {
				if (!store.Has<scene::WeatherCompute>(provider))
					store.Remove<scene::WeatherAcceptedDefinition>(provider);
			});

			store.Each<const scene::WeatherCompute>([&](ecs::Entity provider, const auto &controls) {
				CompileLayer(store, cache, provider, controls, definitionChanges, acceptDesiredEdits);
			});
			if (auto *listener = store.ResourceMutable<WeatherListener>()) {
				const auto retired = [&](std::string_view layerName) {
					return std::none_of(cache.Layers.begin(), cache.Layers.end(), [&](const auto &layer) {
						return layer.Layer == layerName;
					});
				};
				std::erase_if(listener->Thunder.Pending, [&](const auto &arrival) {
					return retired(arrival.Event.Layer);
				});
				std::erase_if(listener->Thunder.Cursors, [&](const auto &cursor) {
					return retired(cursor.Layer);
				});
			}

		} else if (writes != cache.DefinitionWrites) {
			ENGINE_PROFILE("weather changed definitions");
			store.EachChangedBatch<scene::WeatherCompute>([&](const ecs::Entity *providers,
															  const auto *controls,
															  size_t count) {
				for (size_t index = 0; index < count; ++index)
					CompileLayer(store, cache, providers[index], controls[index], true, acceptDesiredEdits);
			});
		}
		bool acceptedPending = false;
		for (auto &layer : cache.Layers) {
			if (!acceptDesiredEdits) break;
			if (!layer.PendingDefinition) continue;
			const auto *clock = store.Get<scene::WeatherClock>(layer.Provider);
			const auto *runtime = store.Get<WeatherRuntimeState>(layer.Provider);
			if (clock && (clock->SimulationTick >= store.Time().Tick ||
						  ((!runtime || !runtime->Ready || runtime->LastTick < clock->Tick) &&
						   !InvalidPhysicalHistory(store, layer.Provider))))
				continue;
			if (const auto *controls = store.Get<scene::WeatherCompute>(layer.Provider)) {
				CompileLayer(store, cache, layer.Provider, *controls, true, true);
				acceptedPending = true;
			}
		}
		if (full || writes != cache.DefinitionWrites || acceptedPending)
			ValidateLayerIdentities(store, cache);
		cache.DefinitionWrites = writes;
		cache.DefinitionMembership = membership;
		cache.NameWrites = names;
		cache.HierarchyWrites = hierarchy;
		cache.AcceptedWrites = store.ComponentChangeVersion<scene::WeatherAcceptedDefinition>();
		cache.AcceptedMembership = store.ComponentMembershipVersion<scene::WeatherAcceptedDefinition>();
		cache.Ready = true;
	}
	std::optional<ConsumerSample> QueryWeather(
		ecs::Store &store, ecs::Entity provider, core::Vector3 positionMetres, float groundHeightMetres
	) {
		PrepareWeatherRuntime(store);
		const auto *controls = scene::WeatherControlsOf(store, provider);
		const auto *cache = store.Resource<WeatherRuntimeCache>();
		const auto layer =
			std::find_if(cache->Layers.begin(), cache->Layers.end(), [&](const auto &prepared) {
				return prepared.Provider == provider;
			});
		if (!controls || layer == cache->Layers.end() || !layer->Compiled) return std::nullopt;
		if (controls->Mode == scene::WeatherMode::Stateful)
			return QueryStatefulWeather(store, provider, positionMetres, groundHeightMetres);
		const auto *clock = store.Get<scene::WeatherClock>(provider);
		if (!clock) return std::nullopt;
		const auto time = WeatherTimeOf(*clock);
		if (!time) return std::nullopt;
		return SampleConsumers(*layer->Compiled, *time, *controls, positionMetres, groundHeightMetres);
	}
	bool ApplyStormDamage(ecs::Store &store, WeatherEventCursor &cursor, const StormEvent &event) {
		if (store.AdoptOnly() || !AcceptWeatherEvent(cursor, event)) return false;
		const float radiusSquared = event.RadiusMetres * event.RadiusMetres;
		store.Each<const scene::Humanoid>([&](ecs::Entity entity, const scene::Humanoid &humanoid) {
			const auto root = humanoid.RootPart == ecs::NULL_ENTITY ? entity : humanoid.RootPart;
			const auto *transform = store.Get<scene::Transform>(root);
			if (!transform ||
				(transform->Frame.Position - event.EndMetres).MagnitudeSquared() > radiusSquared)
				return;
			(void)scene::TakeDamage(store, entity, event.Damage);
		});
		return true;
	}
	void UpdateWeatherRuntime(ecs::Store &store) {
		ENGINE_PROFILE("weather storm runtime");
		PrepareWeatherRuntime(store);
		const auto *cache = store.Resource<WeatherRuntimeCache>();
		for (const auto &layer : cache->Layers) {
			const auto *controlsPointer = scene::WeatherControlsOf(store, layer.Provider);
			const auto *clockPointer = store.Get<scene::WeatherClock>(layer.Provider);
			if (!layer.Compiled || !controlsPointer || !clockPointer ||
				!scene::ValidWeatherClock(*clockPointer))
				continue;
			const auto controls = *controlsPointer;
			const auto clock = *clockPointer;
			if (!store.AdoptOnly()) {
				WeatherRuntimeState state;
				const auto *oldState = store.Get<WeatherRuntimeState>(layer.Provider);
				if (oldState) state = *oldState;
				const WeatherRuntimeState previousState = state;
				WeatherEvents events;
				bool eventsLoaded = false;
				bool eventsChanged = false;
				if (!state.Ready || state.Epoch != clock.Epoch) {
					state = {clock.Epoch, clock.AnchorTick, 1, controls.Seed, 0, 0, true};
					eventsLoaded = true;
					eventsChanged = true;
				}
				if (state.Seed != controls.Seed) {
					state.Seed = controls.Seed;
					state.ChargeQ16 = 0;
					state.HailGrowthQ16 = 0;
				}
				if (controls.LightningRate == 0) state.ChargeQ16 = 0;
				if (controls.HailIntensity == 0) state.HailGrowthQ16 = 0;
				if (clock.Tick < state.LastTick) continue;
				if (!controls.Enabled || clock.Paused || clock.TimeScaleQ16 == 0 ||
					(controls.LightningRate == 0 && controls.HailIntensity == 0)) {
					state.LastTick = clock.Tick;
				} else {
					const uint64_t steps = std::min(MAX_CATCH_UP_TICKS, clock.Tick - state.LastTick);
					for (uint64_t step = 0; step < steps; ++step) {
						const uint64_t tick = state.LastTick + 1;
						if (tick < clock.AnchorTick) {
							// Refuse an externally replaced clock rather than silently discarding old ticks.
							break;
						}
						const auto time = ResolveTime(WeatherAnchorOf(clock), tick);
						if (!time) break;
						core::Vector3 column;
						std::optional<ConsumerSample> sample;
						if (controls.Mode == scene::WeatherMode::Stateful) {
							const auto completed = StatefulWeatherTick(store, layer.Provider);
							const auto *physical = store.Get<StatefulWeatherState>(layer.Provider);
							if (!completed || !physical || time->PhaseTick > *completed) break;
							column = StatefulEventColumn(*physical, time->PhaseTick);
							sample =
								QueryStatefulWeather(store, layer.Provider, column, 0.0f, time->PhaseTick);
						} else {
							column = StormColumn(
								controls.Seed,
								layer.Compiled->Definition.LayerKey,
								tick,
								state.Epoch,
								controls.ExtentMetres
							);
							sample = SampleConsumers(*layer.Compiled, *time, controls, column);
						}

						if (!sample) break;
						// Q16 growth per base-rate tick. Time scale is weather speed, never render delta.
						const float seconds =
							static_cast<float>(clock.TimeScaleQ16) / Q16_ONE / clock.TickRate;
						const uint32_t charge = static_cast<uint32_t>(std::lround(
							std::clamp(
								sample->ChargeSeparation * controls.LightningRate * seconds,
								0.0f,
								static_cast<float>(MAX_LIGHTNING_EVENTS_PER_TICK)
							) *
							Q16_ONE
						));
						const uint32_t hail = static_cast<uint32_t>(std::lround(
							std::clamp(
								sample->HailGrowth * seconds,
								0.0f,
								static_cast<float>(MAX_HAIL_EVENTS_PER_TICK)
							) *
							Q16_ONE
						));
						const uint32_t accumulatedCharge = state.ChargeQ16 + charge;
						const uint32_t accumulatedHail = state.HailGrowthQ16 + hail;
						if (!eventsLoaded && (accumulatedCharge >= Q16_ONE || accumulatedHail >= Q16_ONE)) {
							if (const auto *old = store.Get<WeatherEvents>(layer.Provider)) events = *old;
							if (!events.Events.empty() && events.Events.back().Layer != layer.Layer)
								events = {};
							eventsLoaded = true;
						}
						// Piecewise-constant tick sampling quantizes all crossings to this accepted tick.
						// Lightning precedes hail; sequence orders the copied events without invented
						// sub-tick fields.
						const auto consume =
							[&](uint32_t accumulated, StormEventKind kind, uint32_t maximum) {
								for (uint32_t emitted = 0; emitted < maximum && accumulated >= Q16_ONE;
									 ++emitted) {
									if (!Emit(store, state, events, layer, controls, kind, tick, column))
										break;
									accumulated -= Q16_ONE;
									eventsChanged = true;
								}
								// Missing ground retains one pending crossing, never an unbounded
								// failed-impact bank.
								return std::min(Q16_ONE, accumulated);
							};
						state.ChargeQ16 = consume(
							accumulatedCharge, StormEventKind::Lightning, MAX_LIGHTNING_EVENTS_PER_TICK
						);
						state.HailGrowthQ16 =
							consume(accumulatedHail, StormEventKind::Hail, MAX_HAIL_EVENTS_PER_TICK);
						state.LastTick = tick;
					}
				}
				if (!oldState || state != previousState) store.Set(layer.Provider, state);
				if (eventsChanged) store.Set(layer.Provider, events);
			}
			if (auto *listener = store.ResourceMutable<WeatherListener>()) {
				if (const auto *events = store.Get<WeatherEvents>(layer.Provider)) {
					for (const auto &event : events->Events)
						(void)QueueThunder(
							listener->Thunder,
							event,
							listener->PositionMetres,
							listener->TemperatureCelsius,
							clock.Tick
						);
				}
			}
		}
	}
	void RegisterWeatherRuntimeSystem(ecs::Scheduler &scheduler) {
		scheduler.Add(
			"weather.runtime", ecs::Phase::Animation, UpdateWeatherRuntime, {{}, {}, {}, {"weather.ground"}}
		);
	}
}
