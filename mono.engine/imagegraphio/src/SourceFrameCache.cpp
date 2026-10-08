#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraphio/SourceFrameCache.hpp>

#include <algorithm>
#include <new>

namespace engine::imagegraphio {
	using namespace imagegraph;
	namespace {
		bool Measure(const bake::SurfaceCacheItem &item, uint64_t &tables, uint64_t &pixels) {
			if (item.Elements.size() > (Limits::MaximumEvaluationBytes - tables) / sizeof(SourceArrayItem))
				return false;
			tables += item.Elements.size() * sizeof(SourceArrayItem);
			if (item.Surface.Rgba.capacity() > Limits::MaximumEvaluationBytes - pixels) return false;
			pixels += item.Surface.Rgba.capacity();
			for (const auto &child : item.Elements)
				if (!Measure(child, tables, pixels)) return false;
			return true;
		}
		std::vector<SourceArrayItem> MoveItems(std::vector<bake::SurfaceCacheItem> &source) {
			std::vector<SourceArrayItem> items;
			items.reserve(source.size());
			for (auto &item : source) {
				if (item.IsArray)
					items.push_back({MoveItems(item.Elements)});
				else if (item.Surface.Width) {
					Image image{item.Surface.Width, item.Surface.Height, std::move(item.Surface.Rgba)};
					image.Hash = SurfaceHash(image);
					items.push_back({std::move(image)});
				} else
					items.push_back({ElementValue{int64_t{-4}}});
			}
			return items;
		}
		Value MoveValue(bake::SurfaceCacheItem &item) {
			if (item.IsArray) {
				ArrayValue array{ValueType::Any, {}};
				array.Items = MoveItems(item.Elements);
				return array;
			}
			Image image{item.Surface.Width, item.Surface.Height, std::move(item.Surface.Rgba)};
			image.Hash = SurfaceHash(image);
			return SurfaceValue{std::move(image)};
		}
	}
	Status DecodeSourceFrameCache(
		const Node &node,
		const SourceFrameCacheLayoutObservation &observation,
		DataReplayEntry &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("source frame cache load");
		const auto fail = [&](Status status, std::string message) {
			diagnostic = {status, node.Id, "cache", std::move(message)};
			return status;
		};
		if (maximumBytes > Limits::MaximumEvaluationBytes || node.Id.empty() ||
			node.Id.size() > Limits::MaximumTextBytes ||
			node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
			return fail(Status::LimitExceeded, "source frame cache identity or budget exceeds bounds");
		if (node.Type != "pc.cache" && node.Type != "pc.cache_array")
			return fail(Status::InvalidValue, "saved frame cache requires a source cache node");
		const auto text = SourceFrameCacheSavedText(node);
		if (text.empty()) return fail(Status::InvalidValue, "source frame cache has no enabled saved text");
		bool haveSaved = false, haveSerialize = false;
		for (const auto &property : node.SourceProperties) {
			if (property.Port == "cache") {
				if (haveSaved) return fail(Status::InvalidValue, "saved cache metadata is duplicated");
				haveSaved = true;
			} else if (property.Port == "serialize") {
				if (haveSerialize) return fail(Status::InvalidValue, "Serialize metadata is duplicated");
				haveSerialize = true;
			}
		}
		const auto hash = bake::SpriteCacheDataHash(text);
		if (!hash) return fail(Status::LimitExceeded, "saved frame cache text exceeds bounds");
		if (observation.NodeId != node.Id ||
			observation.DataHash != std::string_view(hash->data(), hash->size()) ||
			bake::SpriteCacheLayoutName(observation.Layout).empty())
			return fail(
				Status::InvalidValue, "saved frame cache needs an exact-data byte layout observation"
			);
		const uint64_t prior = RetainedDataReplayEntryBytes(result);
		const uint64_t identity = sizeof(DataReplayEntry) + node.Id.size() + text.size() +
								  std::string{}.capacity() * 3 + node.Type.size();
		if (prior > maximumBytes || identity > maximumBytes - prior)
			return fail(Status::LimitExceeded, "prior saved frame cache and identity exceed load budget");
		std::vector<bake::SurfaceCacheItem> decoded;
		std::string failure;
		bake::SurfaceCacheFailure failureKind;
		const uint64_t available = maximumBytes - prior - identity;
		if (!bake::ReadSurfaceCache(text, observation.Layout, decoded, failure, available / 2, &failureKind))
			return fail(
				failureKind == bake::SurfaceCacheFailure::LimitExceeded ? Status::LimitExceeded
																		: Status::Malformed,
				std::move(failure)
			);
		if (decoded.size() > Limits::MaximumArrayElements - 2)
			return fail(Status::LimitExceeded, "saved frame cache exceeds native frame bounds");
		uint64_t tables = decoded.capacity() * sizeof(bake::SurfaceCacheItem) +
						  (decoded.size() + 2) * sizeof(DataReplayValueFrame),
				 pixels = 0;
		for (const auto &item : decoded)
			if (!Measure(item, tables, pixels))
				return fail(Status::LimitExceeded, "saved frame cache conversion exceeds byte bounds");
		// Decoder tables, value tables and pixel owners coexist until publication.
		const uint64_t decoderTables =
			bake::SurfaceCacheLimits::MaximumItems * sizeof(bake::SurfaceCacheItem);
		if (tables > available || decoderTables > available - tables ||
			pixels > available - tables - decoderTables)
			return fail(Status::LimitExceeded, "saved frame cache conversion overlaps exceed load budget");
		DataReplayState candidate;
		candidate.Entries.reserve(1);
		DataReplayEntry row;
		row.NodeId = node.Id;
		row.Initialized = true;
		row.PreviousValue = 1;
		row.LoadedCacheData = text;
		row.Values.reserve(decoded.size() + 2);
		row.Values.push_back({0, node.Type});
		row.Values.push_back(
			{1, node.Type == "pc.cache" ? Value{int64_t{-4}} : Value{ArrayValue{ValueType::Any, {}}}}
		);
		for (size_t index = 0; index < decoded.size(); ++index)
			if (decoded[index].IsArray || decoded[index].Surface.Width)
				row.Values.push_back({index + 2, MoveValue(decoded[index])});
		candidate.Entries.push_back(std::move(row));
		if (ValidateDataReplay(candidate, maximumBytes - prior, diagnostic) != Status::Ok)
			return diagnostic.Code;
		core::Metrics::Count("imagegraphio.source_frame_cache_slots", double(decoded.size()));
		result = std::move(candidate.Entries[0]);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, node.Id, "cache", "source frame cache load allocation failed"};
		return diagnostic.Code;
	}
	Status DecodeSourceFrameCaches(
		const Document &document,
		std::span<const SourceFrameCacheLayoutObservation> observations,
		DataReplayState &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, "cache", message};
			return code;
		};
		if (maximumBytes > Limits::MaximumEvaluationBytes || document.Nodes.size() > Limits::MaximumNodes ||
			observations.size() > 64 || result.Entries.size() > Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "saved frame cache load ledger exceeds count bounds");
		uint64_t names = 0;
		const uint64_t scans = std::max<size_t>(1, observations.size());
		const uint64_t maximumNames = (64ull * 1024 * 1024) / scans;
		for (const auto &node : document.Nodes) {
			if (node.Id.size() > Limits::MaximumTextBytes ||
				node.SourceProperties.size() > Limits::MaximumPropertiesPerNode ||
				node.Id.size() + 1 > maximumNames - std::min(names, maximumNames))
				return fail(Status::LimitExceeded, "saved frame cache node lookup exceeds work bounds");
			names += node.Id.size() + 1;
		}
		for (const auto &observation : observations) {
			if (observation.NodeId.empty() || observation.NodeId.size() > Limits::MaximumTextBytes ||
				observation.DataHash.size() != 64 || bake::SpriteCacheLayoutName(observation.Layout).empty())
				return fail(Status::InvalidValue, "saved cache layout observation is malformed");
			if (observation.NodeId.size() + 1 > maximumNames - std::min(names, maximumNames))
				return fail(Status::LimitExceeded, "saved cache observation lookup exceeds work bounds");
			names += observation.NodeId.size() + 1;
		}
		const auto prior = RetainedDataReplayBytes(result);
		const auto table = observations.size() * sizeof(DataReplayEntry) + sizeof(DataReplayState);
		if (prior > maximumBytes || table > maximumBytes - prior)
			return fail(Status::LimitExceeded, "saved frame cache load table exceeds byte budget");
		for (size_t index = 0; index < observations.size(); ++index)
			for (size_t earlier = 0; earlier < index; ++earlier)
				if (observations[index].NodeId == observations[earlier].NodeId)
					return fail(Status::DuplicateId, "saved cache layout observations repeat a node");
		DataReplayState candidate;
		candidate.Entries.reserve(observations.size());
		for (const auto &node : document.Nodes) {
			if ((node.Type != "pc.cache" && node.Type != "pc.cache_array") ||
				SourceFrameCacheSavedText(node).empty())
				continue;
			const auto found = std::find_if(observations.begin(), observations.end(), [&](const auto &item) {
				return item.NodeId == node.Id;
			});
			if (found == observations.end()) continue;
			const auto retained = RetainedDataReplayBytes(candidate);
			if (retained > maximumBytes - prior)
				return fail(Status::LimitExceeded, "saved frame cache load ledger exceeds byte budget");
			DataReplayEntry row;
			if (DecodeSourceFrameCache(node, *found, row, diagnostic, maximumBytes - prior - retained) !=
				Status::Ok)
				return diagnostic.Code;
			candidate.Entries.push_back(std::move(row));
		}
		if (ValidateDataReplay(candidate, maximumBytes - prior, diagnostic) != Status::Ok)
			return diagnostic.Code;
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, "cache", "saved frame cache ledger allocation failed"};
		return diagnostic.Code;
	}

	Status CookSourceFrameCaches(
		const Document &source,
		std::span<const SourceFrameCacheLayoutObservation> observations,
		Document &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraphio.source_frame_cache_cook");
		const auto fail = [&](Status code, const char *message) {
			diagnostic = {code, {}, "cache", message};
			return code;
		};
		if (maximumBytes > Limits::MaximumEvaluationBytes || source.FormatVersion < 9 ||
			observations.empty() || observations.size() > 64)
			return fail(
				Status::InvalidValue, "cache cooking needs native source metadata and explicit layouts"
			);
		const auto prior = DocumentRetainedPayloadBytes(result);
		const auto sourceBytes = DocumentRetainedPayloadBytes(source);
		if (!prior || !sourceBytes || *prior > maximumBytes || *sourceBytes > (maximumBytes - *prior) / 2)
			return fail(Status::LimitExceeded, "cache cook document generations exceed byte budget");
		// Input, previous destination and its candidate remain alive throughout the transaction.
		const uint64_t available = maximumBytes - *prior - *sourceBytes * 2;
		DataReplayState loads;
		if (DecodeSourceFrameCaches(source, observations, loads, diagnostic, available) != Status::Ok)
			return diagnostic.Code;
		if (loads.Entries.size() != observations.size())
			return fail(Status::InvalidValue, "cache cook receipt does not identify an enabled source cache");
		const uint64_t rowBytes = RetainedDataReplayBytes(loads);
		if (rowBytes > available)
			return fail(Status::LimitExceeded, "cache cook loaded rows exceed byte budget");
		uint64_t remaining = available - rowBytes;
		// Reserve both sides of property-table growth and the exact-text identity before cloning.
		for (const auto &row : loads.Entries) {
			const auto node = std::find_if(source.Nodes.begin(), source.Nodes.end(), [&](const auto &item) {
				return item.Id == row.NodeId;
			});
			if (node == source.Nodes.end())
				return fail(Status::InvalidValue, "cache cook source identity disappeared");
			const auto old = std::count_if(
				node->SourceProperties.begin(), node->SourceProperties.end(), [](const auto &property) {
					return property.Port == SOURCE_FRAME_CACHE_NATIVE_TEXT ||
						   property.Port == SOURCE_FRAME_CACHE_NATIVE_DATA;
				}
			);
			if (node->SourceProperties.size() - old + 2 > Limits::MaximumPropertiesPerNode)
				return fail(Status::LimitExceeded, "cache cook annotations exceed property count bounds");
			const uint64_t extra = 2 * (node->SourceProperties.size() + 2) * sizeof(AuthoredValue) +
								   row.LoadedCacheData.size() + 256;
			if (extra > remaining)
				return fail(Status::LimitExceeded, "cache cook annotations exceed byte budget");
			remaining -= extra;
		}
		Document candidate = source;
		for (const auto &row : loads.Entries) {
			ArrayValue packet;
			if (EncodeSourceFrameCacheReceipt(row, packet, diagnostic, remaining) != Status::Ok)
				return diagnostic.Code;
			const auto packetBytes = ValueClonePayloadBytes(packet);
			if (!packetBytes || *packetBytes > remaining)
				return fail(Status::LimitExceeded, "cache cook packet exceeds byte budget");
			remaining -= *packetBytes;
			auto node = std::find_if(candidate.Nodes.begin(), candidate.Nodes.end(), [&](const auto &item) {
				return item.Id == row.NodeId;
			});
			std::erase_if(node->SourceProperties, [](const auto &property) {
				return property.Port == SOURCE_FRAME_CACHE_NATIVE_TEXT ||
					   property.Port == SOURCE_FRAME_CACHE_NATIVE_DATA;
			});
			node->SourceProperties.reserve(node->SourceProperties.size() + 2);
			node->SourceProperties.push_back(
				{std::string(SOURCE_FRAME_CACHE_NATIVE_TEXT), row.LoadedCacheData}
			);
			node->SourceProperties.push_back(
				{std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), std::move(packet)}
			);
		}
		const auto candidateBytes = DocumentRetainedPayloadBytes(candidate);
		if (!candidateBytes || *candidateBytes > maximumBytes - *prior - *sourceBytes - rowBytes)
			return fail(Status::LimitExceeded, "cache cook retained capacities exceed byte budget");
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, "cache", "cache cook allocation failed"};
		return diagnostic.Code;
	}

}
