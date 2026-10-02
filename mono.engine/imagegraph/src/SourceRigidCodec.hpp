#pragma once
#include "SourceRigidPayload.hpp"

#include <istream>
#include <ostream>
namespace engine::imagegraph::detail {
	// Native WriteQuoted/ReadQuoted remain the single text escape implementation.
	template <class WriteString>
	void WriteSourceRigidAlias(std::ostream &stream, const RigidValue &value, WriteString writeString) {
		if (!value.Data) {
			stream << 0;
			return;
		}
		stream << 1 << ' ' << value.Data->OwnerId.size() << ' ' << value.Data->BodyId.size() << ' ';
		writeString(stream, value.Data->OwnerId);
		stream << ' ';
		writeString(stream, value.Data->BodyId);
	}
	// ReadString enforces its supplied decoded-byte cap before each string growth.
	template <class Admit, class ReadString>
	bool ReadSourceRigidAlias(std::istream &stream, RigidValue &value, Admit admit, ReadString readString) {
		int present = 0;
		if (!(stream >> present) || (present != 0 && present != 1)) return false;
		if (!present) {
			value = {};
			return true;
		}
		size_t ownerBytes = 0, bodyBytes = 0;
		if (!(stream >> ownerBytes >> bodyBytes) || !ownerBytes || ownerBytes > Limits::MaximumTextBytes ||
			!bodyBytes || bodyBytes > 256)
			return false;
		// Two strings may grow geometrically; include their small-string capacity
		// before allocation.
		if (!admit(sizeof(RigidObjectData) + 2 * (ownerBytes + bodyBytes) + 32)) return false;
		RigidObjectData data;
		if (!readString(stream, data.OwnerId, ownerBytes) || data.OwnerId.size() != ownerBytes ||
			!readString(stream, data.BodyId, bodyBytes) || data.BodyId.size() != bodyBytes)
			return false;
		RigidValue candidate;
		candidate.Data.emplace() = std::move(data);
		if (!ValidRigidPayload(candidate)) return false;
		value = std::move(candidate);
		return true;
	}
} // namespace engine::imagegraph::detail
