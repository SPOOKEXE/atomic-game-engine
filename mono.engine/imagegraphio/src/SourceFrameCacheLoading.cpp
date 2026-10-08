#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>
#include <engine/imagegraphio/SourceFrameCacheLoading.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <nlohmann/json.hpp>

namespace engine::imagegraphio {
	using namespace imagegraph;
	namespace {
		// SAX keeps no DOM. Array-child accounting includes holes and empty nested arrays.
		struct SlotCounter final : nlohmann::json_sax<nlohmann::json> {
			std::array<bool, bake::SurfaceCacheLimits::MaximumDepth + 2> Arrays{};
			size_t Depth = 0;
			uint64_t Slots = 0, Items = 0;
			bool RootArray = false, Limited = false;
			bool Value() {
				if (!Depth) return false;
				if (!Arrays[Depth - 1]) return true;
				if (++Items > bake::SurfaceCacheLimits::MaximumItems) {
					Limited = true;
					return false;
				}
				if (Depth == 1) ++Slots;
				return true;
			}
			bool Begin(bool array) {
				if (Depth == Arrays.size()) {
					Limited = true;
					return false;
				}
				if (!Depth) {
					if (!array || RootArray) return false;
					RootArray = true;
				} else if (!Value()) {
					return false;
				}
				Arrays[Depth++] = array;
				return true;
			}
			bool null() override {
				return Value();
			}
			bool boolean(bool) override {
				return Value();
			}
			bool number_integer(number_integer_t) override {
				return Value();
			}
			bool number_unsigned(number_unsigned_t) override {
				return Value();
			}
			bool number_float(number_float_t, const string_t &) override {
				return Value();
			}
			bool string(string_t &) override {
				return Value();
			}
			bool binary(binary_t &) override {
				return false;
			}
			bool start_object(std::size_t) override {
				return Begin(false);
			}
			bool key(string_t &) override {
				return true;
			}
			bool end_object() override {
				--Depth;
				return true;
			}
			bool start_array(std::size_t) override {
				return Begin(true);
			}
			bool end_array() override {
				--Depth;
				return true;
			}
			bool parse_error(std::size_t, const std::string &, const nlohmann::detail::exception &) override {
				return false;
			}
		};
		Status CountSlots(const Node &node, uint64_t available, uint64_t &slots, Diagnostic &diagnostic) {
			const auto fail = [&](Status code, const char *message) {
				diagnostic = {code, node.Id, "cache", message};
				return code;
			};
			const auto text = SourceFrameCacheSavedText(node);
			if (text.empty()) return fail(Status::InvalidValue, "source cache has no enabled saved text");
			// Lexer strings and their temporary input buffer are bounded before parsing begins.
			if (text.size() > bake::SpriteCacheLimits::MaximumEncodedBytes ||
				available < sizeof(SlotCounter) || text.size() > (available - sizeof(SlotCounter)) / 4)
				return fail(Status::LimitExceeded, "source cache slot scan exceeds byte bounds");
			SlotCounter counter;
			if (!nlohmann::json::sax_parse(text.begin(), text.end(), &counter) || !counter.RootArray ||
				counter.Depth)
				return fail(
					counter.Limited ? Status::LimitExceeded : Status::Malformed,
					"source cache indexed-array slot scan refused its JSON shape"
				);
			slots = counter.Slots;
			return Status::Ok;
		}
	}

	Status DecodeSourceFrameCacheLoading(
		const Node &node,
		const SourceFrameCacheLayoutObservation &observation,
		DataReplayEntry &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		const auto prior = RetainedDataReplayEntryBytes(result);
		if (maximumBytes > Limits::MaximumEvaluationBytes || prior > maximumBytes ||
			node.Id.size() > Limits::MaximumTextBytes ||
			node.SourceProperties.size() > Limits::MaximumPropertiesPerNode) {
			diagnostic = {
				Status::LimitExceeded, node.Id, "cache", "source cache loading exceeds byte bounds"
			};
			return diagnostic.Code;
		}
		uint64_t slots = 0;
		if (CountSlots(node, maximumBytes - prior, slots, diagnostic) != Status::Ok) return diagnostic.Code;
		const auto status = DecodeSourceFrameCache(node, observation, result, diagnostic, maximumBytes);
		if (status == Status::Ok) result.SourceFrameCacheSerializedSlots = slots;
		return status;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, node.Id, "cache", "source cache slot scan allocation failed"};
		return diagnostic.Code;
	}

	Status DecodeSourceFrameCachesLoading(
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
		const auto prior = RetainedDataReplayBytes(result);
		constexpr uint64_t scanTableBytes = sizeof(std::array<uint64_t, 64>);
		if (maximumBytes > Limits::MaximumEvaluationBytes || prior > maximumBytes ||
			scanTableBytes > maximumBytes - prior || document.Nodes.size() > Limits::MaximumNodes ||
			observations.size() > 64)
			return fail(Status::LimitExceeded, "source cache inventory exceeds scan bounds");
		uint64_t nameWork = 0;
		for (const auto &node : document.Nodes) {
			if (node.Id.size() > Limits::MaximumTextBytes ||
				node.SourceProperties.size() > Limits::MaximumPropertiesPerNode ||
				node.Id.size() > SOURCE_FRAME_CACHE_EDIT_WORK_BYTES - nameWork)
				return fail(Status::LimitExceeded, "source cache names exceed scan bounds");
			nameWork += node.Id.size();
		}
		if (!observations.empty() && nameWork > SOURCE_FRAME_CACHE_EDIT_WORK_BYTES / observations.size())
			return fail(Status::LimitExceeded, "source cache name comparisons exceed scan bounds");
		std::array<uint64_t, 64> slots{};
		uint64_t work = 0;
		for (size_t index = 0; index < observations.size(); ++index) {
			if (observations[index].NodeId.size() > Limits::MaximumTextBytes)
				return fail(Status::LimitExceeded, "source cache observation identity exceeds scan bounds");
			for (const auto &node : document.Nodes) {
				if (node.Id.size() > Limits::MaximumTextBytes ||
					node.SourceProperties.size() > Limits::MaximumPropertiesPerNode)
					return fail(Status::LimitExceeded, "source cache node exceeds scan bounds");
				if (node.Id != observations[index].NodeId) continue;
				const auto text = SourceFrameCacheSavedText(node);
				if (text.size() > SOURCE_FRAME_CACHE_EDIT_WORK_BYTES - work)
					return fail(Status::LimitExceeded, "source cache scan work exceeds bounds");
				work += text.size();
				if (CountSlots(node, maximumBytes - prior - scanTableBytes, slots[index], diagnostic) !=
					Status::Ok)
					return diagnostic.Code;
				break;
			}
		}
		const auto status = DecodeSourceFrameCaches(
			document, observations, result, diagnostic, maximumBytes - scanTableBytes
		);
		if (status != Status::Ok) return status;
		for (auto &row : result.Entries)
			for (size_t index = 0; index < observations.size(); ++index)
				if (row.NodeId == observations[index].NodeId) {
					row.SourceFrameCacheSerializedSlots = slots[index];
					break;
				}
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, {}, "cache", "source cache slot scan allocation failed"};
		return diagnostic.Code;
	}
	Status CookSourceFrameCachesLoading(
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
		if (DecodeSourceFrameCachesLoading(source, observations, loads, diagnostic, available) != Status::Ok)
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
