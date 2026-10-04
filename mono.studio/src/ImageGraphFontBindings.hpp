#pragma once

#include <engine/imagegraph/DataReplay.hpp>
#include <engine/imagegraph/FeedbackHost.hpp>
#include <engine/imagegraph/SourceFont.hpp>
#include <engine/imagegraphfont/GraphFontInputs.hpp>

#include <algorithm>
#include <new>

namespace studio::detail {
	struct ImageGraphTextFontSeedView {
		const engine::imagegraph::FontValue *Primary = nullptr;
		const engine::imagegraph::FontValue *Fallback = nullptr;
	};

	inline bool ImageGraphTextFontFreezeWorkWithinBounds(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::DataReplayState &replay,
		const engine::imagegraphfont::GraphFontConfiguration &configuration,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		constexpr uint64_t maximumWeightedWork = 16 * 1024 * 1024;
		const auto fail = [&] {
			diagnostic = {
				Status::LimitExceeded,
				{},
				"font inputs",
				"Text font replay selection exceeds its bounded work budget"
			};
			return false;
		};
		uint64_t textCount = 0, textIdBytes = 0, replayCount = replay.Entries.size(), replayIdBytes = 0;
		uint64_t seedCount = configuration.Context.InitialTextFonts.size(), seedIdBytes = 0;
		for (const auto &node : document.Nodes)
			if (node.Type == "pc.text") {
				if (textCount == Limits::MaximumNodes || node.Id.size() > maximumWeightedWork - textIdBytes)
					return fail();
				++textCount;
				textIdBytes += node.Id.size();
			}
		for (const auto &entry : replay.Entries) {
			if (entry.NodeId.size() > maximumWeightedWork - replayIdBytes) return fail();
			replayIdBytes += entry.NodeId.size();
		}
		for (const auto &seed : configuration.Context.InitialTextFonts) {
			if (seed.NodeId.size() > maximumWeightedWork - seedIdBytes) return fail();
			seedIdBytes += seed.NodeId.size();
		}
		uint64_t work = 0;
		const auto addProduct = [&](uint64_t left, uint64_t right, uint64_t weight = 1) {
			if (!left || !right || !weight) return true;
			if (left > maximumWeightedWork / weight || right > maximumWeightedWork / (left * weight))
				return false;
			const uint64_t amount = left * right * weight;
			if (amount > maximumWeightedWork - work) return false;
			work += amount;
			return true;
		};
		if (!addProduct(textCount, replayCount, 2) || !addProduct(textIdBytes, replayCount, 2) ||
			!addProduct(replayIdBytes, textCount, 2) || !addProduct(textCount, seedCount) ||
			!addProduct(textIdBytes, seedCount) || !addProduct(seedIdBytes, textCount))
			return fail();
		return true;
	}

	inline bool ReadImageGraphTextFontSeed(
		const engine::imagegraph::DataReplayEntry *entry,
		const engine::imagegraph::Node &node,
		engine::imagegraph::FrameTime frame,
		ImageGraphTextFontSeedView &seed,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		if (!entry) return true;
		if (!entry->Initialized || entry->Values.empty() || entry->Tick != frame.Tick ||
			entry->Subframe != frame.Subframe || entry->NegativeFrame != frame.NegativeFrame) {
			diagnostic = {
				Status::InvalidValue, node.Id, "font inputs", "Text replay does not match the held frame"
			};
			return false;
		}
		const auto *record = std::get_if<StructValue>(&entry->Values.back().Data);
		if (!record || !record->Data) {
			diagnostic = {
				Status::InvalidValue, node.Id, "font inputs", "Text replay font state is malformed"
			};
			return false;
		}
		bool havePrimary = false, haveFallback = false;
		for (const auto &[name, value] : record->Data->Fields) {
			const bool primary = name == "primary", fallback = name == "fallback";
			if (!primary && !fallback) continue;
			bool &seen = primary ? havePrimary : haveFallback;
			if (seen) {
				diagnostic = {
					Status::InvalidValue, node.Id, "font inputs", "Text replay repeats a font field"
				};
				return false;
			}
			seen = true;
			const auto *font = std::get_if<FontValue>(&value);
			if (font) {
				const auto bytes = SourceFontValueRetainedBytes(*font);
				if (!bytes) {
					diagnostic = {Status::InvalidValue, node.Id, name, "Text replay font payload is invalid"};
					return false;
				}
				(primary ? seed.Primary : seed.Fallback) = font;
			} else if (!std::holds_alternative<UndefinedValue>(value)) {
				diagnostic = {
					Status::InvalidValue, node.Id, name, "Text replay font field has the wrong type"
				};
				return false;
			}
		}
		if (!havePrimary || !haveFallback) {
			diagnostic = {Status::InvalidValue, node.Id, "font inputs", "Text replay omits a font field"};
			return false;
		}
		return true;
	}

	inline bool ImageGraphFontConfigurationFitsBudget(
		const engine::imagegraphfont::GraphFontConfiguration &configuration,
		uint64_t heldBytes,
		uint64_t maximumBytes,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		const auto candidateBytes =
			engine::imagegraphfont::GraphFontConfigurationRetainedBytes(configuration);
		if (!candidateBytes || heldBytes > maximumBytes || *candidateBytes > maximumBytes - heldBytes) {
			diagnostic = {
				engine::imagegraph::Status::LimitExceeded,
				{},
				"font inputs",
				"Held and frozen font configurations exceed their byte budget"
			};
			return false;
		}
		return true;
	}

	inline bool FreezeImageGraphInitialTextFontsFromReplay(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::DataReplayState &replay,
		engine::imagegraph::FrameTime frame,
		engine::imagegraphfont::GraphFontConfiguration &configuration,
		uint64_t maximumBytes,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		using namespace engine::imagegraph;
		try {
			const auto configurationBytes =
				engine::imagegraphfont::GraphFontConfigurationRetainedBytes(configuration);
			const uint64_t replayBytes = RetainedDataReplayBytes(replay);
			if (!configurationBytes || replayBytes > maximumBytes ||
				*configurationBytes > (maximumBytes - replayBytes) / 2) {
				diagnostic = {
					Status::LimitExceeded,
					{},
					"font inputs",
					"Prepared replay and font configuration exceed their byte budget"
				};
				return false;
			}
			if (ValidateDataReplay(replay, maximumBytes - *configurationBytes, diagnostic) != Status::Ok)
				return false;
			if (replay.Entries.size() > UINT64_MAX / sizeof(size_t)) {
				diagnostic = {
					Status::LimitExceeded, {}, "font inputs", "Replay validation workspace exceeds its budget"
				};
				return false;
			}
			const uint64_t validationWorkspace = uint64_t(replay.Entries.size()) * sizeof(size_t);
			if (validationWorkspace > maximumBytes - replayBytes - 2 * *configurationBytes) {
				diagnostic = {
					Status::LimitExceeded, {}, "font inputs", "Replay validation workspace exceeds its budget"
				};
				return false;
			}
			if (!ImageGraphTextFontFreezeWorkWithinBounds(document, replay, configuration, diagnostic))
				return false;
			uint64_t addedBytes = 0;
			size_t addedSeeds = 0;
			const auto add = [&](uint64_t amount) {
				if (addedBytes > maximumBytes || amount > maximumBytes - addedBytes) return false;
				addedBytes += amount;
				return true;
			};
			for (const auto &node : document.Nodes) {
				if (node.Type != "pc.text") continue;
				const auto replayEntry =
					std::find_if(replay.Entries.begin(), replay.Entries.end(), [&](const auto &item) {
						return item.NodeId == node.Id && item.ProcessorRow == 0;
					});
				if (replayEntry == replay.Entries.end()) continue;
				ImageGraphTextFontSeedView seed;
				if (!ReadImageGraphTextFontSeed(&*replayEntry, node, frame, seed, diagnostic)) return false;
				const auto existing = std::find_if(
					configuration.Context.InitialTextFonts.begin(),
					configuration.Context.InitialTextFonts.end(),
					[&](const auto &item) { return item.NodeId == node.Id; }
				);
				if (existing != configuration.Context.InitialTextFonts.end()) {
					const auto same = [](const std::optional<FontValue> &stored, const FontValue *incoming) {
						return stored.has_value() == (incoming != nullptr) &&
							   (!incoming || *stored == *incoming);
					};
					if (!same(existing->Primary, seed.Primary) || !same(existing->Fallback, seed.Fallback)) {
						diagnostic = {
							Status::InvalidValue,
							node.Id,
							"font inputs",
							"Configured text seed conflicts with prepared replay"
						};
						return false;
					}
					continue;
				}
				if (!add(sizeof(SourceFontInitialTextState)) || !add(uint64_t(node.Id.size()) + 1)) {
					diagnostic = {
						Status::LimitExceeded,
						node.Id,
						"font inputs",
						"Text font seed metadata exceeds its budget"
					};
					return false;
				}
				for (const auto *font : {seed.Primary, seed.Fallback})
					if (font) {
						const auto fontBytes = SourceFontValueRetainedBytes(*font);
						if (!fontBytes || !add(*fontBytes)) {
							diagnostic = {
								Status::LimitExceeded,
								node.Id,
								"font inputs",
								"Text font seed exceeds its byte budget"
							};
							return false;
						}
					}
				++addedSeeds;
			}
			const size_t priorSeedCount = configuration.Context.InitialTextFonts.size();
			if (addedSeeds > Limits::MaximumNodes - std::min<size_t>(priorSeedCount, Limits::MaximumNodes) ||
				*configurationBytes > (maximumBytes - replayBytes) / 2 ||
				priorSeedCount + addedSeeds > UINT64_MAX / (2 * sizeof(SourceFontInitialTextState))) {
				diagnostic = {
					Status::LimitExceeded,
					{},
					"font inputs",
					"Prepared text font seeds exceed their byte budget"
				};
				return false;
			}
			const uint64_t vectorPeak =
				uint64_t(priorSeedCount + addedSeeds) * 2 * sizeof(SourceFontInitialTextState);
			const uint64_t twoConfigurations = *configurationBytes * 2;
			const uint64_t baseAndReplay = replayBytes + twoConfigurations + validationWorkspace;
			if (addedBytes > maximumBytes - baseAndReplay ||
				vectorPeak > maximumBytes - baseAndReplay - addedBytes) {
				diagnostic = {
					Status::LimitExceeded,
					{},
					"font inputs",
					"Prepared text font seeds exceed their byte budget"
				};
				return false;
			}
			if (!addedSeeds) {
				diagnostic = {};
				return true;
			}
			auto candidate = configuration;
			candidate.Context.InitialTextFonts.reserve(priorSeedCount + addedSeeds);
			for (const auto &node : document.Nodes) {
				if (node.Type != "pc.text") continue;
				const auto matchingEntry =
					std::find_if(replay.Entries.begin(), replay.Entries.end(), [&](const auto &item) {
						return item.NodeId == node.Id && item.ProcessorRow == 0;
					});
				if (matchingEntry == replay.Entries.end()) continue;
				ImageGraphTextFontSeedView seed;
				if (!ReadImageGraphTextFontSeed(&*matchingEntry, node, frame, seed, diagnostic)) return false;
				const auto existing = std::find_if(
					candidate.Context.InitialTextFonts.begin(),
					candidate.Context.InitialTextFonts.end(),
					[&](const auto &item) { return item.NodeId == node.Id; }
				);
				if (existing != candidate.Context.InitialTextFonts.end()) continue;
				candidate.Context.InitialTextFonts.emplace_back();
				auto &copy = candidate.Context.InitialTextFonts.back();
				copy.NodeId = node.Id;
				if (seed.Primary) copy.Primary = *seed.Primary;
				if (seed.Fallback) copy.Fallback = *seed.Fallback;
			}
			const auto originalBytes =
				engine::imagegraphfont::GraphFontConfigurationRetainedBytes(configuration);
			const auto candidateBytes =
				engine::imagegraphfont::GraphFontConfigurationRetainedBytes(candidate);
			if (!originalBytes || !candidateBytes || replayBytes > maximumBytes ||
				*originalBytes > maximumBytes - replayBytes ||
				*candidateBytes > maximumBytes - replayBytes - *originalBytes) {
				diagnostic = {
					Status::LimitExceeded,
					{},
					"font inputs",
					"Live font configuration, candidate and replay exceed their byte budget"
				};
				return false;
			}
			configuration = std::move(candidate);
			diagnostic = {};
			return true;
		} catch (const std::bad_alloc &) {
			diagnostic = {
				Status::LimitExceeded, {}, "font inputs", "Frozen text font seed allocation was refused"
			};
			return false;
		}
	}

	inline bool FreezePreparedImageGraphInitialTextFonts(
		const engine::imagegraph::Document &document,
		const engine::imagegraph::CapturedFeedbackHost &feedback,
		uint64_t revision,
		uint64_t inputRevision,
		engine::imagegraph::FrameTime heldFrame,
		engine::imagegraphfont::GraphFontConfiguration &configuration,
		uint64_t maximumBytes,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		const auto prepared = feedback.PreparedFrame(revision, inputRevision);
		if (!prepared || prepared->Tick != heldFrame.Tick || prepared->Subframe != heldFrame.Subframe ||
			prepared->NegativeFrame != heldFrame.NegativeFrame) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				{},
				"font inputs",
				"No matching prepared replay is available for the held export frame"
			};
			return false;
		}
		const auto *replay = feedback.PreparedData(revision, inputRevision);
		if (!replay) {
			diagnostic = {
				engine::imagegraph::Status::InvalidValue,
				{},
				"font inputs",
				"Prepared font replay is no longer available"
			};
			return false;
		}
		return FreezeImageGraphInitialTextFontsFromReplay(
			document, *replay, heldFrame, configuration, maximumBytes, diagnostic
		);
	}

	inline bool BindImageGraphFontInputs(
		const engine::imagegraphfont::GraphFontInputs &inputs,
		bool active,
		bool playing,
		engine::imagegraph::SourceFontContext &heldContext,
		engine::imagegraph::EvaluationRequest &request,
		uint64_t maximumBytes,
		engine::imagegraph::Diagnostic &diagnostic
	) {
		if (!active) return true;
		return inputs.Bind(playing, heldContext, request, maximumBytes, diagnostic);
	}
} // namespace studio::detail
