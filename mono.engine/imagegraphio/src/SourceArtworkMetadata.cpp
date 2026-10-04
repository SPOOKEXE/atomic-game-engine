#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraphio/SourceArtworkEdit.hpp>

#include <algorithm>
#include <cmath>

namespace engine::imagegraphio {
	using namespace imagegraph;
	namespace {
		const Value *Field(const StructValue &value, std::string_view name) {
			if (!value.Data) return nullptr;
			for (const auto &[key, field] : value.Data->Fields)
				if (key == name) return &field;
			return nullptr;
		}
		const Value *Port(std::span<const AuthoredValue> values, std::string_view name) {
			for (const auto &value : values)
				if (value.Port == name) return &value.Data;
			return nullptr;
		}
		const ArrayValue *Array(const StructValue &object, std::string_view name) {
			const auto *value = Field(object, name);
			const auto *array = value ? std::get_if<ArrayValue>(value) : nullptr;
			return array && array->Nested.empty() && (array->Items.empty() || array->Elements.empty())
					   ? array
					   : nullptr;
		}
		size_t Count(const ArrayValue &array) {
			return array.Items.empty() ? array.Elements.size() : array.Items.size();
		}
		const StructValue *Object(const ArrayValue &array, size_t index) {
			if (array.Items.empty()) return std::get_if<StructValue>(&array.Elements[index]);
			const auto *leaf = std::get_if<ElementValue>(&array.Items[index].Data);
			return leaf ? std::get_if<StructValue>(leaf) : nullptr;
		}
		std::optional<uint64_t> Number(const Value *value) {
			if (!value) return std::nullopt;
			if (const auto *integer = std::get_if<int64_t>(value)) {
				if (*integer >= 0) return uint64_t(*integer);
			} else if (const auto *number = std::get_if<double>(value)) {
				if (std::isfinite(*number) && *number >= 0 && *number <= 9007199254740991. &&
					std::floor(*number) == *number)
					return uint64_t(*number);
			}
			return std::nullopt;
		}
		const std::string *Text(const StructValue &value, std::string_view name) {
			const auto *field = Field(value, name);
			return field ? std::get_if<std::string>(field) : nullptr;
		}
		uint64_t Bytes(const SourceArtworkMetadata &value) {
			uint64_t bytes = sizeof(value) + value.Layers.capacity() * sizeof(SourceArtworkLayer) +
							 value.Tags.capacity() * sizeof(SourceArtworkTag);
			for (const auto &layer : value.Layers)
				bytes += layer.Name.capacity() + 1;
			for (const auto &tag : value.Tags)
				bytes += tag.Name.capacity() + 1;
			return bytes;
		}
		bool Spend(uint64_t bytes, uint64_t &remaining) {
			if (bytes > remaining) return false;
			remaining -= bytes;
			return true;
		}
		bool Name(std::string_view source, std::string &out, uint64_t &remaining) {
			if (source.size() > Limits::MaximumTextBytes ||
				!Spend(std::max(source.size(), out.capacity()) + 1, remaining))
				return false;
			out = source;
			return out.capacity() <= std::max(source.size(), std::string{}.capacity()) ||
				   Spend(out.capacity() - std::max(source.size(), std::string{}.capacity()), remaining);
		}
		template <class T> bool Slots(std::vector<T> &out, size_t count, uint64_t &remaining) {
			if (count > Limits::MaximumArrayElements || !Spend(count * sizeof(T), remaining)) return false;
			out.reserve(count);
			return Spend((out.capacity() - count) * sizeof(T), remaining);
		}
		// The source map keys the ORIGINAL name. A suffix collision is deliberately retained.
		template <class T> bool Normalize(std::vector<T> &items, uint64_t &remaining, uint64_t &work) {
			for (size_t i = items.size(); i-- > 0;) {
				size_t duplicates = 0;
				for (size_t j = 0; j < i; ++j) {
					const uint64_t comparisons =
						1 + (items[j].Name.size() == items[i].Name.size() ? items[i].Name.size() : 0);
					if (!Spend(comparisons, work)) return false;
					if (items[j].Name == items[i].Name) ++duplicates;
				}
				if (!duplicates) continue;
				const auto suffix = "_" + std::to_string(duplicates);
				const uint64_t admitted =
					std::max(items[i].Name.size() + suffix.size(), std::string{}.capacity());
				if (items[i].Name.size() > Limits::MaximumTextBytes - suffix.size() ||
					!Spend(admitted + 1, remaining))
					return false;
				std::string candidate = items[i].Name + suffix;
				if (candidate.capacity() > admitted && !Spend(candidate.capacity() - admitted, remaining))
					return false;
				items[i].Name = std::move(candidate);
			}
			return true;
		}
	}
	static Status ReadMetadata(
		const Node &authored,
		const StructValue &contentObject,
		std::string_view kind,
		uint64_t borrowed,
		SourceArtworkMetadata &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraphio.artwork_metadata");
		const auto fail = [&](Status code, std::string message) {
			diagnostic = {code, authored.Id, "content", std::move(message)};
			return code;
		};
		uint64_t remaining = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (!Spend(borrowed, remaining) || !Spend(Bytes(result), remaining) ||
			!Spend(sizeof(SourceArtworkMetadata), remaining))
			return fail(Status::LimitExceeded, "prepared artwork metadata overlap exceeds operation bounds");
		const auto *content = &contentObject;
		if (!content->Data) return fail(Status::InvalidValue, "prepared artwork content is absent");
		SourceArtworkMetadata candidate;
		if (kind == "pc.ora_file_read" || kind == "pc.krita_file_read") {
			// The granted native layered-file host normalizes source ORA.layerData and Krita.layerDat.
			const auto *layers = Array(*content, "layerData");
			if (!layers || !Slots(candidate.Layers, Count(*layers), remaining))
				return fail(Status::LimitExceeded, "prepared artwork layer slots exceed operation bounds");
			for (size_t i = 0; i < Count(*layers); ++i) {
				const auto *layer = Object(*layers, i);
				const auto *name = layer ? Text(*layer, "name") : nullptr;
				SourceArtworkLayer entry;
				if (!name || !Name(*name, entry.Name, remaining))
					return fail(Status::InvalidValue, "prepared artwork layer name is invalid");
				candidate.Layers.push_back(std::move(entry));
			}
		} else if (kind == "pc.ase_file_read") {
			const auto framesCount = Number(Field(*content, "Frame amount"));
			const auto *frames = Array(*content, "Frames");
			if (!framesCount || !*framesCount || !frames || Count(*frames) != *framesCount ||
				Count(*frames) > Limits::MaximumArrayElements)
				return fail(Status::InvalidValue, "prepared ASE frame metadata is invalid");
			uint64_t scans = 0;
			size_t layerCount = 0;
			for (size_t f = 0; f < Count(*frames); ++f) {
				const auto *frame = Object(*frames, f);
				const auto *chunks = frame ? Array(*frame, "Chunks") : nullptr;
				if (!chunks || Count(*chunks) > Limits::MaximumArrayElements ||
					Count(*chunks) > 1'048'576 - scans)
					return fail(Status::LimitExceeded, "prepared ASE chunk scan exceeds operation bounds");
				scans += Count(*chunks);
				for (size_t c = 0; c < Count(*chunks); ++c) {
					const auto *chunk = Object(*chunks, c);
					const auto type = chunk ? Number(Field(*chunk, "Type")) : std::nullopt;
					if (!type) return fail(Status::InvalidValue, "prepared ASE chunk type is invalid");
					if (*type == 0x2004) ++layerCount;
				}
			}
			if (!Slots(candidate.Layers, layerCount, remaining))
				return fail(Status::LimitExceeded, "prepared ASE layer slots exceed operation bounds");
			candidate.Frames = *framesCount;
			const ArrayValue *tags = nullptr;
			const auto *loopValue = Port(authored.Values, "attribute_layer_loop");
			const auto *loops = loopValue ? std::get_if<ArrayValue>(loopValue) : nullptr;
			uint64_t work = 0;
			for (size_t f = 0; f < Count(*frames); ++f) {
				const auto *frame = Object(*frames, f);
				const auto *chunks = frame ? Array(*frame, "Chunks") : nullptr;
				if (!chunks || Count(*chunks) > Limits::MaximumArrayElements ||
					Count(*chunks) > 1'048'576 - work)
					return fail(Status::LimitExceeded, "prepared ASE chunk work exceeds operation bounds");
				work += Count(*chunks);
				for (size_t c = 0; c < Count(*chunks); ++c) {
					const auto *chunk = Object(*chunks, c);
					const auto type = chunk ? Number(Field(*chunk, "Type")) : std::nullopt;
					if (!type) return fail(Status::InvalidValue, "prepared ASE chunk type is invalid");
					if (*type == 0x2004) {
						const auto *name = Text(*chunk, "Name");
						const auto layerType = Number(Field(*chunk, "Layer type"));
						SourceArtworkLayer entry;
						if (!name || !layerType || candidate.Layers.size() >= Limits::MaximumArrayElements ||
							!Name(*name, entry.Name, remaining))
							return fail(Status::InvalidValue, "prepared ASE layer metadata is invalid");
						entry.Renderable = *layerType == 0;
						if (loops && candidate.Layers.size() < loops->Elements.size()) {
							const auto *loop = std::get_if<bool>(&loops->Elements[candidate.Layers.size()]);
							if (!loop)
								return fail(Status::InvalidValue, "prepared ASE layer loop is not Boolean");
							entry.Loop = *loop;
						}
						candidate.Layers.push_back(std::move(entry));
					} else if (*type == 0x2005 && f > 0) {
						const auto layer = Number(Field(*chunk, "Layer index"));
						if (!layer || *layer >= candidate.Layers.size())
							return fail(Status::InvalidValue, "prepared ASE cel layer index is invalid");
						candidate.Layers[size_t(*layer)].Loop = false;
					} else if (*type == 0x2018 && f == 0)
						tags = Array(*chunk, "Tags");
				}
			}
			if (tags) {
				if (!Slots(candidate.Tags, Count(*tags), remaining))
					return fail(Status::LimitExceeded, "prepared ASE tag slots exceed operation bounds");
				for (size_t t = 0; t < Count(*tags); ++t) {
					const auto *tag = Object(*tags, t);
					const auto *name = tag ? Text(*tag, "Name") : nullptr;
					const auto first = tag ? Number(Field(*tag, "Frame start")) : std::nullopt;
					const auto last = tag ? Number(Field(*tag, "Frame end")) : std::nullopt;
					const auto color = tag ? Number(Field(*tag, "Color")) : std::nullopt;
					SourceArtworkTag entry;
					if (!tag || !first || !last || *first > 65535 || *last > 65535 || !color ||
						*color > 0xffffff || !Name(name ? *name : "region", entry.Name, remaining))
						return fail(Status::InvalidValue, "prepared ASE tag metadata is invalid");
					entry.First = *first;
					entry.Last = *last;
					entry.Color = {uint8_t(*color), uint8_t(*color >> 8), uint8_t(*color >> 16), 255};
					candidate.Tags.push_back(std::move(entry));
				}
			}
			uint64_t nameWork = 16'777'216;
			if (!Normalize(candidate.Layers, remaining, nameWork) ||
				!Normalize(candidate.Tags, remaining, nameWork))
				return fail(
					Status::LimitExceeded,
					"prepared ASE normalized names exceed byte or comparison-work bounds"
				);
		} else
			return fail(Status::UnknownNode, "artwork action requires ASE ORA or Krita file content");
		core::Metrics::Count("imagegraphio.artwork_layers", candidate.Layers.size());
		core::Metrics::Count("imagegraphio.artwork_metadata_bytes", Bytes(candidate));
		result = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, authored.Id, "content", "artwork metadata allocation failed"};
		return diagnostic.Code;
	}
	Status ReadSourceArtworkMetadata(
		const HostNodeCapture &prepared,
		SourceArtworkMetadata &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		const auto borrowed = HostCaptureRetainedPayloadBytes(prepared);
		const auto *value = Port(prepared.Outputs, "content");
		const auto *content = value ? std::get_if<StructValue>(value) : nullptr;
		if (!borrowed || !content || prepared.State != HostCaptureState::Recorded) {
			diagnostic = {
				Status::InvalidValue,
				prepared.Authored.Id,
				"content",
				"artwork action requires bounded successful prepared file content"
			};
			return diagnostic.Code;
		}
		return ReadMetadata(
			prepared.Authored, *content, prepared.Authored.Type, *borrowed, result, diagnostic, maximumBytes
		);
	}
	Status ReadSourceAsepriteMetadata(
		const Node &controls,
		const Value &contentValue,
		SourceArtworkMetadata &result,
		Diagnostic &diagnostic,
		uint64_t maximumBytes
	) {
		const auto *content = std::get_if<StructValue>(&contentValue);
		const auto nodeBytes = NodeClonePayloadBytes(controls);
		const auto valueBytes = ValueClonePayloadBytes(contentValue);
		if (!content || !nodeBytes || !valueBytes || *nodeBytes > UINT64_MAX - *valueBytes) {
			diagnostic = {
				Status::InvalidValue,
				controls.Id,
				"content",
				"source ASE profile requires bounded owned inspection content"
			};
			return diagnostic.Code;
		}
		return ReadMetadata(
			controls, *content, "pc.ase_file_read", *nodeBytes + *valueBytes, result, diagnostic, maximumBytes
		);
	}

}
