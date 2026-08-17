#include <engine/core/Bytes.hpp>
#include <engine/game/Content.hpp>

namespace engine::game {

	namespace {
		std::vector<std::byte> Finish(const core::ByteWriter &writer) {
			const std::span<const std::byte> bytes = writer.Bytes();
			return {bytes.begin(), bytes.end()};
		}

		// Reads the tag and refuses anything that is not the one expected.
		//
		// **Every decoder here starts with this**, because both directions of
		// the user channel are shared: a payload that is somebody else's message
		// must be a non-event rather than a misread.
		bool Tagged(core::ByteReader &reader, PlayMessage expected) {
			const uint8_t tag = reader.ReadUInt8();
			return !reader.Failed() && tag == static_cast<uint8_t>(expected);
		}
	}

	std::vector<std::byte> EncodeContentDirectory(const ContentDirectory &directory) {
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::ContentDirectory));

		const size_t named = std::min(directory.Endpoints.size(), MAXIMUM_CONTENT_ENDPOINTS);
		writer.WriteUInt16(static_cast<uint16_t>(named));
		for (size_t index = 0; index < named; ++index) {
			const ContentEndpoint &endpoint = directory.Endpoints[index];
			writer.WriteString(endpoint.Name);
			writer.WriteString(endpoint.Kind);
			writer.WriteString(endpoint.Location);
		}

		writer.WriteUInt32(static_cast<uint32_t>(directory.Grant.size()));
		writer.WriteRaw(directory.Grant.data(), directory.Grant.size());
		writer.WriteString(directory.PublisherKey);
		return Finish(writer);
	}

	bool DecodeContentDirectory(std::span<const std::byte> message, ContentDirectory &out) {
		core::ByteReader reader(message);
		if (!Tagged(reader, PlayMessage::ContentDirectory)) {
			return false;
		}

		const uint16_t named = reader.ReadUInt16();
		if (reader.Failed() || named > MAXIMUM_CONTENT_ENDPOINTS) {
			return false;
		}

		ContentDirectory directory;
		directory.Endpoints.reserve(named);
		for (uint16_t index = 0; index < named; ++index) {
			ContentEndpoint endpoint;
			endpoint.Name = std::string(reader.ReadString());
			endpoint.Kind = std::string(reader.ReadString());
			endpoint.Location = std::string(reader.ReadString());
			if (reader.Failed()) {
				return false;
			}
			directory.Endpoints.push_back(std::move(endpoint));
		}

		const uint32_t grantBytes = reader.ReadUInt32();
		if (reader.Failed()) {
			return false;
		}
		directory.Grant.resize(grantBytes);
		if (grantBytes != 0 && !reader.ReadRaw(directory.Grant.data(), grantBytes)) {
			return false;
		}
		directory.PublisherKey = std::string(reader.ReadString());
		if (reader.Failed()) {
			return false;
		}

		out = std::move(directory);
		return true;
	}

	std::vector<std::byte> EncodeContentRequest(const ContentRouteRequest &request) {
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::ContentRequest));
		writer.WriteUInt64(request.Ticket);
		writer.WriteString(request.Route);
		return Finish(writer);
	}

	bool DecodeContentRequest(std::span<const std::byte> message, ContentRouteRequest &out) {
		core::ByteReader reader(message);
		if (!Tagged(reader, PlayMessage::ContentRequest)) {
			return false;
		}

		ContentRouteRequest request;
		request.Ticket = reader.ReadUInt64();
		const std::string_view route = reader.ReadString();
		if (reader.Failed() || route.size() > MAXIMUM_ROUTE_BYTES) {
			return false;
		}

		request.Route = std::string(route);
		out = std::move(request);
		return true;
	}

	std::vector<std::byte> EncodeContentChunk(const ContentChunk &chunk) {
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::ContentChunk));
		writer.WriteUInt64(chunk.Ticket);
		writer.WriteUInt32(chunk.TotalBytes);
		writer.WriteUInt32(chunk.Offset);
		writer.WriteUInt32(static_cast<uint32_t>(chunk.Bytes.size()));
		writer.WriteRaw(chunk.Bytes.data(), chunk.Bytes.size());
		return Finish(writer);
	}

	bool DecodeContentChunk(std::span<const std::byte> message, ContentChunk &out) {
		core::ByteReader reader(message);
		if (!Tagged(reader, PlayMessage::ContentChunk)) {
			return false;
		}

		ContentChunk chunk;
		chunk.Ticket = reader.ReadUInt64();
		chunk.TotalBytes = reader.ReadUInt32();
		chunk.Offset = reader.ReadUInt32();
		const uint32_t carried = reader.ReadUInt32();
		if (reader.Failed() || carried > MAXIMUM_CONTENT_CHUNK_BYTES) {
			return false;
		}

		// **Checked here rather than by the reassembler**, because there is one
		// decoder and there will be more than one caller. A piece that starts or
		// ends past the total it names is either a corrupt message or a peer
		// asking a receiver to write outside a buffer it sized from the same
		// message, and both are refused the same way.
		if (static_cast<uint64_t>(chunk.Offset) + carried > chunk.TotalBytes) {
			return false;
		}

		chunk.Bytes.resize(carried);
		if (carried != 0 && !reader.ReadRaw(chunk.Bytes.data(), carried)) {
			return false;
		}

		out = std::move(chunk);
		return true;
	}

	std::vector<std::byte> EncodeContentRefusal(const ContentRefusal &refusal) {
		core::ByteWriter writer;
		writer.WriteUInt8(static_cast<uint8_t>(PlayMessage::ContentRefusal));
		writer.WriteUInt64(refusal.Ticket);
		return Finish(writer);
	}

	bool DecodeContentRefusal(std::span<const std::byte> message, ContentRefusal &out) {
		core::ByteReader reader(message);
		if (!Tagged(reader, PlayMessage::ContentRefusal)) {
			return false;
		}

		ContentRefusal refusal;
		refusal.Ticket = reader.ReadUInt64();
		if (reader.Failed()) {
			return false;
		}

		out = refusal;
		return true;
	}
}
