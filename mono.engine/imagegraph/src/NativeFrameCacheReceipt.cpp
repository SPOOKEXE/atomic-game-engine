#include "NativeFrameCacheReceipt.hpp"

#include "ValuePayload.hpp"

#include <engine/core/Bytes.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/FrameCacheReplay.hpp>

#include <algorithm>
#include <cstring>
#include <new>

namespace engine::imagegraph {
	namespace {
		constexpr size_t CHUNK_BYTES = 64 * 1024;
		struct Shape {
			uint64_t Encoded = 0, Decoded = 0;
			size_t Nodes = 0;
			std::optional<uint64_t> SerializedSlots;
			uint64_t SelectedDecoded = 0;
			bool Add(uint64_t bytes) {
				if (bytes > Limits::MaximumArrayBytes - std::min(Encoded, Limits::MaximumArrayBytes))
					return false;
				Encoded += bytes;
				return true;
			}
		};
		bool PutItem(const SourceArrayItem &item, Shape &shape, core::ByteWriter *writer, size_t depth);
		bool PutSurface(const Image &image, Shape &shape, core::ByteWriter *writer) {
			if (image.Format != SurfaceFormat::RGBA8Unorm ||
				!ValidSurfaceLayout(image, Limits::MaximumDimension, Limits::MaximumArrayBytes) ||
				!shape.Add(9 + image.Pixels.size()))
				return false;
			if (writer) {
				writer->WriteUInt8(1);
				writer->WriteUInt32(image.Width);
				writer->WriteUInt32(image.Height);
				writer->WriteRaw(image.Pixels.data(), image.Pixels.size());
			}
			return true;
		}
		bool PutArray(
			const std::vector<SourceArrayItem> &items, Shape &shape, core::ByteWriter *writer, size_t depth
		) {
			if (depth > Limits::MaximumArrayDepth || items.size() > Limits::MaximumArrayElements ||
				!shape.Add(5))
				return false;
			if (writer) {
				writer->WriteUInt8(2);
				writer->WriteUInt32(uint32_t(items.size()));
			}
			for (const auto &item : items)
				if (!PutItem(item, shape, writer, depth + 1)) return false;
			return true;
		}
		bool PutItem(const SourceArrayItem &item, Shape &shape, core::ByteWriter *writer, size_t depth) {
			if (depth > Limits::MaximumArrayDepth || ++shape.Nodes > Limits::MaximumArrayElements)
				return false;
			if (const auto *image = std::get_if<Image>(&item.Data)) return PutSurface(*image, shape, writer);
			if (const auto *items = std::get_if<std::vector<SourceArrayItem>>(&item.Data))
				return PutArray(*items, shape, writer, depth);
			const auto *missing = std::get_if<int64_t>(&std::get<ElementValue>(item.Data));
			if (!missing || *missing != -4 || !shape.Add(1)) return false;
			if (writer) writer->WriteUInt8(0);
			return true;
		}
		bool PutValue(const Value &value, Shape &shape, core::ByteWriter *writer) {
			if (++shape.Nodes > Limits::MaximumArrayElements) return false;
			if (const auto *surface = std::get_if<SurfaceValue>(&value))
				return PutSurface(surface->Data, shape, writer);
			if (const auto *array = std::get_if<ArrayValue>(&value)) {
				if (array->ElementType != ValueType::Any || !array->Elements.empty() ||
					!array->Nested.empty())
					return false;
				return PutArray(array->Items, shape, writer, 1);
			}
			const auto *missing = std::get_if<int64_t>(&value);
			if (!missing || *missing != -4 || !shape.Add(1)) return false;
			if (writer) writer->WriteUInt8(0);
			return true;
		}
		struct Cursor {
			const std::vector<ElementValue> &Chunks;
			size_t Chunk = 0, Offset = 0;
			uint64_t Remaining;
			bool Good = true;
			bool Raw(void *target, size_t bytes) {
				if (!Good || bytes > Remaining) {
					Good = false;
					return false;
				}
				auto *destination = static_cast<uint8_t *>(target);
				while (bytes) {
					const auto &text = std::get<std::string>(Chunks[Chunk]);
					const size_t count = std::min(bytes, text.size() - Offset);
					if (destination) {
						std::memcpy(destination, text.data() + Offset, count);
						destination += count;
					}
					bytes -= count;
					Remaining -= count;
					Offset += count;
					if (Offset == text.size()) {
						++Chunk;
						Offset = 0;
					}
				}
				return true;
			}
			uint8_t Byte() {
				uint8_t value = 0;
				Raw(&value, 1);
				return value;
			}
			uint32_t UInt() {
				uint32_t value = 0;
				for (unsigned i = 0; i < 4; ++i)
					value |= uint32_t(Byte()) << (8 * i);
				return value;
			}
			bool Name(std::string_view name) {
				if (UInt() != name.size()) return false;
				for (const unsigned char byte : name)
					if (Byte() != byte) return false;
				return Good;
			}
		};
		bool GetItem(
			Cursor &cursor, SourceArrayItem *result, Shape &shape, size_t depth, uint8_t *rootTag = nullptr
		) {
			if (depth > Limits::MaximumArrayDepth || ++shape.Nodes > Limits::MaximumArrayElements)
				return false;
			const auto tag = cursor.Byte();
			if (!cursor.Good) return false;
			if (rootTag) *rootTag = tag;
			if (tag == 0) {
				if (result) result->Data = ElementValue{int64_t{-4}};
				return true;
			}
			if (tag == 1) {
				const auto width = cursor.UInt(), height = cursor.UInt();
				const auto layout =
					CheckedSurfaceLayout(width, height, SurfaceFormat::RGBA8Unorm, Limits::MaximumArrayBytes);
				if (!cursor.Good || width > Limits::MaximumDimension || height > Limits::MaximumDimension ||
					!layout || layout->Bytes > cursor.Remaining ||
					layout->Bytes >
						Limits::MaximumArrayBytes - std::min(shape.Decoded, Limits::MaximumArrayBytes))
					return false;
				shape.Decoded += layout->Bytes;
				if (!result) return cursor.Raw(nullptr, layout->Bytes);
				Image image;
				image.Width = width;
				image.Height = height;
				image.Pixels.resize(layout->Bytes);
				if (!cursor.Raw(image.Pixels.data(), image.Pixels.size())) return false;
				image.Hash = SurfaceHash(image);
				result->Data = std::move(image);
				return true;
			}
			if (tag != 2) return false;
			const auto count = cursor.UInt();
			if (!cursor.Good || count > Limits::MaximumArrayElements - shape.Nodes ||
				uint64_t(count) * sizeof(SourceArrayItem) >
					Limits::MaximumArrayBytes - std::min(shape.Decoded, Limits::MaximumArrayBytes))
				return false;
			shape.Decoded += uint64_t(count) * sizeof(SourceArrayItem);
			std::vector<SourceArrayItem> items;
			if (result) items.resize(count);
			for (uint32_t i = 0; i < count; ++i)
				if (!GetItem(cursor, result ? &items[i] : nullptr, shape, depth + 1)) return false;
			if (result) result->Data = std::move(items);
			return true;
		}
		bool Packet(
			const ArrayValue &chunks,
			uint64_t bytes,
			const Node &node,
			Shape &shape,
			DataReplayEntry *row,
			std::optional<uint64_t> requestedFrame = {},
			bool *hasFrame = nullptr,
			SourceArrayItem *selected = nullptr
		) {
			Cursor cursor{chunks.Elements, 0, 0, bytes};
			const auto version = cursor.UInt();
			if ((version != 1 && version != 2) || !cursor.Name(node.Type)) return false;
			if (version == 2) {
				const auto slots = cursor.UInt();
				if (!cursor.Good || slots > Limits::MaximumArrayElements) return false;
				shape.SerializedSlots = slots;
			}
			const auto count = cursor.UInt();
			if (!cursor.Good || count > Limits::MaximumArrayElements - 2) return false;
			shape.Decoded = sizeof(DataReplayEntry) + node.Id.size() + SourceFrameCacheIdentity(node).size() +
							(count + 2) * sizeof(DataReplayValueFrame) + node.Type.size() + 128;
			if (row) {
				row->NodeId = node.Id;
				row->Initialized = true;
				row->PreviousValue = 1;
				row->LoadedCacheData = SourceFrameCacheIdentity(node);
				row->SourceFrameCacheSerializedSlots = shape.SerializedSlots;
				row->Values.reserve(count + 2);
				row->Values.push_back({0, node.Type});
				row->Values.push_back(
					{1, node.Type == "pc.cache" ? Value{int64_t{-4}} : Value{ArrayValue{ValueType::Any, {}}}}
				);
			}
			uint32_t previous = 1;
			for (uint32_t i = 0; i < count; ++i) {
				const auto frame = cursor.UInt();
				if (!cursor.Good || frame <= previous ||
					(shape.SerializedSlots ? uint64_t(frame) - 2 >= *shape.SerializedSlots
										   : frame >= Limits::MaximumArrayElements))
					return false;
				previous = frame;
				SourceArrayItem item;
				uint8_t rootTag = 0;
				const uint64_t before = shape.Decoded;
				SourceArrayItem *destination = row ? &item : (requestedFrame == frame ? selected : nullptr);
				if (!GetItem(cursor, destination, shape, 1, &rootTag)) return false;
				if (requestedFrame == frame) shape.SelectedDecoded = shape.Decoded - before;
				if (hasFrame && requestedFrame == frame && (rootTag == 1 || rootTag == 2)) *hasFrame = true;
				if (!row) continue;
				Value value;
				if (auto *image = std::get_if<Image>(&item.Data))
					value = SurfaceValue{std::move(*image)};
				else if (auto *items = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
					ArrayValue array{ValueType::Any, {}};
					array.Items = std::move(*items);
					value = std::move(array);
				} else
					value = std::get<int64_t>(std::get<ElementValue>(item.Data));
				row->Values.push_back({frame, std::move(value)});
			}
			return cursor.Good && !cursor.Remaining;
		}
		Status
		Metadata(const Node &node, const ArrayValue *&chunks, uint64_t &bytes, Diagnostic *diagnostic) {
			const auto fail = [&](Status status, const char *message) {
				if (diagnostic)
					*diagnostic = {status, node.Id, std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), message};
				return status;
			};
			if (node.Id.empty() || node.SourceProperties.size() > Limits::MaximumPropertiesPerNode ||
				node.Id.size() > Limits::MaximumTextBytes)
				return fail(Status::LimitExceeded, "native frame cache metadata exceeds bounds");
			const std::string *identity = nullptr;
			bool savedSeen = false, serializeSeen = false;
			chunks = nullptr;
			bytes = 0;
			for (const auto &property : node.SourceProperties) {
				if (property.Port == "cache") {
					if (savedSeen || !std::holds_alternative<std::string>(property.Data))
						return fail(Status::InvalidValue, "saved cache metadata is invalid or repeated");
					savedSeen = true;
				} else if (property.Port == "serialize") {
					if (serializeSeen || !std::holds_alternative<bool>(property.Data))
						return fail(Status::InvalidValue, "Serialize metadata is invalid or repeated");
					serializeSeen = true;
				} else if (property.Port == SOURCE_FRAME_CACHE_NATIVE_TEXT) {
					if (identity || !(identity = std::get_if<std::string>(&property.Data)))
						return fail(
							Status::InvalidValue, "native frame cache identity is invalid or repeated"
						);
				} else if (property.Port == SOURCE_FRAME_CACHE_NATIVE_DATA) {
					if (chunks || !(chunks = std::get_if<ArrayValue>(&property.Data)))
						return fail(Status::InvalidValue, "native frame cache packet is invalid or repeated");
				}
			}
			if (!identity && !chunks)
				return fail(Status::UnsupportedExecution, "no cooked frame cache receipt");
			if (!identity || !chunks || identity->empty() || identity->size() > Limits::MaximumTextBytes ||
				*identity != SourceFrameCacheIdentity(node) ||
				(node.Type != "pc.cache" && node.Type != "pc.cache_array") ||
				chunks->ElementType != ValueType::Text || !chunks->Items.empty() || !chunks->Nested.empty() ||
				chunks->Elements.empty() ||
				chunks->Elements.size() > Limits::MaximumArrayBytes / CHUNK_BYTES + 1)
				return fail(Status::InvalidValue, "native frame cache receipt is incomplete or stale");
			for (const auto &element : chunks->Elements) {
				const auto *chunk = std::get_if<std::string>(&element);
				if (!chunk || chunk->empty() || chunk->size() > CHUNK_BYTES ||
					chunk->size() > Limits::MaximumArrayBytes - bytes)
					return fail(Status::LimitExceeded, "native frame cache chunks exceed bounds");
				bytes += chunk->size();
			}
			if (chunks->Elements.size() * sizeof(ElementValue) > Limits::MaximumArrayBytes - bytes)
				return fail(Status::LimitExceeded, "native frame cache chunk table exceeds value bounds");
			return Status::Ok;
		}
	}
	Status detail::InspectNativeFrameCacheReceipt(
		const Node &node, uint64_t tick, NativeFrameCacheReceiptInspection &inspection
	) {
		ENGINE_PROFILE("imagegraph.frame_cache.inspect_receipt");
		const ArrayValue *chunks;
		uint64_t bytes;
		const auto status = Metadata(node, chunks, bytes, nullptr);
		if (status != Status::Ok) return status;
		NativeFrameCacheReceiptInspection candidate;
		const std::optional<uint64_t> requestedFrame =
			tick <= UINT64_MAX - 2 ? std::optional<uint64_t>{tick + 2} : std::nullopt;
		Shape shape;
		if (!Packet(*chunks, bytes, node, shape, nullptr, requestedFrame, &candidate.HasFrame))
			return Status::Malformed;
		candidate.DecodedBytes = shape.Decoded;
		candidate.SerializedSlots = shape.SerializedSlots;
		inspection = candidate;
		return Status::Ok;
	}
	Status detail::DecodeSourceFrameCacheReceiptSlot(
		const Node &node, uint64_t slot, Value &value, Diagnostic &diagnostic, uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.decode_receipt_slot");
		const auto fail = [&](Status status, const char *message) {
			diagnostic = {status, node.Id, std::string(SOURCE_FRAME_CACHE_NATIVE_DATA), message};
			return status;
		};
		const ArrayValue *chunks;
		uint64_t bytes;
		const auto metadata = Metadata(node, chunks, bytes, &diagnostic);
		if (metadata != Status::Ok) return metadata;
		Shape measured;
		const auto frame = slot <= UINT64_MAX - 2 ? std::optional<uint64_t>{slot + 2} : std::nullopt;
		if (!Packet(*chunks, bytes, node, measured, nullptr, frame))
			return fail(Status::Malformed, "native frame cache packet is malformed");
		if (!measured.SerializedSlots)
			return fail(
				Status::UnsupportedExecution,
				"progressive native loading requires original serialized slot count"
			);
		if (slot >= *measured.SerializedSlots)
			return fail(
				Status::InvalidValue, "native frame cache slot is outside original serialized inventory"
			);
		if (!detail::ValidRuntimeValue(value))
			return fail(Status::InvalidValue, "prior native frame cache slot value is invalid");
		const auto prior = ValueClonePayloadBytes(value);
		if (!prior || maximumBytes > Limits::MaximumEvaluationBytes || *prior > maximumBytes ||
			measured.SelectedDecoded + sizeof(Value) + 128 > maximumBytes - *prior)
			return fail(Status::LimitExceeded, "native frame cache slot decoding exceeds byte budget");
		SourceArrayItem item{ElementValue{int64_t{-4}}};
		Shape decoded;
		if (!Packet(*chunks, bytes, node, decoded, nullptr, frame, nullptr, &item))
			return fail(Status::Malformed, "native frame cache packet changed during slot decoding");
		Value candidate;
		if (auto *image = std::get_if<Image>(&item.Data))
			candidate = SurfaceValue{std::move(*image)};
		else if (auto *items = std::get_if<std::vector<SourceArrayItem>>(&item.Data)) {
			ArrayValue array{ValueType::Any, {}};
			array.Items = std::move(*items);
			candidate = std::move(array);
		} else
			candidate = std::get<int64_t>(std::get<ElementValue>(item.Data));
		const auto retained = ValueClonePayloadBytes(candidate);
		if (!detail::ValidRuntimeValue(candidate) || !retained || *retained > maximumBytes - *prior)
			return fail(Status::LimitExceeded, "native frame cache slot capacities exceed byte budget");
		value = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, node.Id, {}, "native frame cache slot allocation failed"};
		return diagnostic.Code;
	}

	Status EncodeSourceFrameCacheReceipt(
		const DataReplayEntry &row, ArrayValue &chunks, Diagnostic &diagnostic, uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.encode_receipt");
		const auto fail = [&](Status status, const char *message) {
			diagnostic = {status, row.NodeId, {}, message};
			return status;
		};
		const auto type = SourceFrameCacheRowType(row);
		if (!row.Initialized || row.NodeId.empty() || row.NodeId.size() > Limits::MaximumTextBytes ||
			row.LoadedCacheData.size() > Limits::MaximumTextBytes || type.empty() ||
			row.Values.size() > Limits::MaximumArrayElements || row.LoadedCacheData.empty())
			return fail(Status::InvalidValue, "native frame cache needs a decoded constructor row");
		if (row.SourceFrameCacheLoading)
			return fail(
				Status::UnsupportedExecution, "native frame cache receipt needs completed constructor loading"
			);
		if (row.SourceFrameCacheSerializedSlots &&
			*row.SourceFrameCacheSerializedSlots > Limits::MaximumArrayElements)
			return fail(Status::LimitExceeded, "native frame cache serialized slot count exceeds bounds");
		const uint64_t headerBytes = (row.SourceFrameCacheSerializedSlots ? 16 : 12) + type.size();
		Shape shape;
		shape.Encoded = headerBytes;
		uint64_t previous = 1;
		for (size_t i = 2; i < row.Values.size(); ++i) {
			if (row.Values[i].Frame <= previous ||
				(row.SourceFrameCacheSerializedSlots
					 ? row.Values[i].Frame - 2 >= *row.SourceFrameCacheSerializedSlots
					 : row.Values[i].Frame >= Limits::MaximumArrayElements) ||
				!shape.Add(4) || !PutValue(row.Values[i].Data, shape, nullptr))
				return fail(
					Status::InvalidValue, "native frame cache surface tree or frame order is invalid"
				);
			previous = row.Values[i].Frame;
		}
		const auto priorPayload = ValueClonePayloadBytes(chunks);
		if (!priorPayload) return fail(Status::InvalidValue, "prior native chunks are invalid");
		const auto prior = *priorPayload;
		const uint64_t count = (shape.Encoded + CHUNK_BYTES - 1) / CHUNK_BYTES;
		const uint64_t tableBytes = count * sizeof(ElementValue);
		if (tableBytes > Limits::MaximumArrayBytes - shape.Encoded)
			return fail(
				Status::LimitExceeded, "native frame cache chunk table exceeds authored value bounds"
			);
		const uint64_t needed = shape.Encoded * 2 + tableBytes + 128;
		if (maximumBytes > Limits::MaximumEvaluationBytes || prior > maximumBytes ||
			needed > maximumBytes - prior)
			return fail(Status::LimitExceeded, "native frame cache encoding exceeds byte budget");
		core::ByteWriter writer(shape.Encoded, shape.Encoded);
		writer.WriteUInt32(row.SourceFrameCacheSerializedSlots ? 2 : 1);
		writer.WriteString(type);
		if (row.SourceFrameCacheSerializedSlots)
			writer.WriteUInt32(uint32_t(*row.SourceFrameCacheSerializedSlots));
		writer.WriteUInt32(uint32_t(row.Values.size() - 2));
		Shape written;
		written.Encoded = headerBytes;
		for (size_t i = 2; i < row.Values.size(); ++i) {
			writer.WriteUInt32(uint32_t(row.Values[i].Frame));
			written.Add(4);
			if (!PutValue(row.Values[i].Data, written, &writer))
				return fail(Status::InvalidValue, "native frame cache changed during encoding");
		}
		ArrayValue candidate{ValueType::Text, {}};
		candidate.Elements.reserve(count);
		const auto bytes = writer.Bytes();
		for (size_t offset = 0; offset < bytes.size(); offset += CHUNK_BYTES)
			candidate.Elements.push_back(
				std::string(
					reinterpret_cast<const char *>(bytes.data() + offset),
					std::min(CHUNK_BYTES, bytes.size() - offset)
				)
			);
		const auto retained = ValueClonePayloadBytes(candidate);
		if (!detail::ValidPayload(candidate, false) || !retained ||
			*retained + writer.Bytes().size() > maximumBytes - prior)
			return fail(Status::LimitExceeded, "native frame cache chunk capacities exceed byte budget");
		chunks = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, row.NodeId, {}, "native frame cache allocation failed"};
		return diagnostic.Code;
	}
	Status MeasureSourceFrameCacheReceipt(const Node &node, uint64_t &decodedBytes, Diagnostic &diagnostic) {
		ENGINE_PROFILE("imagegraph.frame_cache.measure_receipt");
		const ArrayValue *chunks;
		uint64_t bytes;
		const auto status = Metadata(node, chunks, bytes, &diagnostic);
		if (status != Status::Ok) return status;
		Shape shape;
		if (!Packet(*chunks, bytes, node, shape, nullptr)) {
			diagnostic = {Status::Malformed, node.Id, {}, "native frame cache packet is malformed"};
			return diagnostic.Code;
		}
		decodedBytes = shape.Decoded;
		diagnostic = {};
		return Status::Ok;
	}
	Status DecodeSourceFrameCacheReceipt(
		const Node &node, DataReplayEntry &row, Diagnostic &diagnostic, uint64_t maximumBytes
	) try {
		ENGINE_PROFILE("imagegraph.frame_cache.decode_receipt");
		uint64_t decoded;
		const auto measured = MeasureSourceFrameCacheReceipt(node, decoded, diagnostic);
		if (measured != Status::Ok) return measured;
		if (row.Values.size() > Limits::MaximumArrayElements ||
			row.NodeId.size() > Limits::MaximumTextBytes ||
			row.LoadedCacheData.size() > Limits::MaximumTextBytes) {
			diagnostic = {Status::LimitExceeded, node.Id, {}, "prior native frame cache exceeds bounds"};
			return diagnostic.Code;
		}
		for (const auto &frame : row.Values)
			if (!detail::ValidRuntimeValue(frame.Data)) {
				diagnostic = {Status::InvalidValue, node.Id, {}, "prior native frame cache value is invalid"};
				return diagnostic.Code;
			}
		const auto prior = RetainedDataReplayEntryBytes(row);
		if (maximumBytes > Limits::MaximumEvaluationBytes || prior > maximumBytes ||
			decoded > maximumBytes - prior) {
			diagnostic = {
				Status::LimitExceeded, node.Id, {}, "native frame cache decode exceeds byte budget"
			};
			return diagnostic.Code;
		}
		const ArrayValue *chunks;
		uint64_t bytes;
		if (Metadata(node, chunks, bytes, &diagnostic) != Status::Ok) return diagnostic.Code;
		DataReplayEntry candidate;
		Shape shape;
		if (!Packet(*chunks, bytes, node, shape, &candidate)) {
			diagnostic = {
				Status::Malformed, node.Id, {}, "native frame cache packet changed during decoding"
			};
			return diagnostic.Code;
		}
		if (RetainedDataReplayEntryBytes(candidate) > std::min(decoded, maximumBytes - prior)) {
			diagnostic = {
				Status::LimitExceeded, node.Id, {}, "native frame cache capacities exceed byte budget"
			};
			return diagnostic.Code;
		}
		for (const auto &frame : candidate.Values)
			if (!detail::ValidRuntimeValue(frame.Data)) {
				diagnostic = {Status::InvalidValue, node.Id, {}, "native frame cache value is invalid"};
				return diagnostic.Code;
			}
		row = std::move(candidate);
		diagnostic = {};
		return Status::Ok;
	} catch (const std::bad_alloc &) {
		diagnostic = {Status::LimitExceeded, node.Id, {}, "native frame cache allocation failed"};
		return diagnostic.Code;
	}
}
