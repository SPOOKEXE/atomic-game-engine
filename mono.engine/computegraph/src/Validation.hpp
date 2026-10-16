#pragma once

#include <engine/computegraph/Document.hpp>

#include <algorithm>

namespace engine::computegraph::detail {
	inline Status AllocationFailure(Diagnostic &diagnostic) noexcept {
		diagnostic.Code = Status::AllocationFailed;
		diagnostic.Subject.clear();
		return Status::AllocationFailed;
	}
	inline Status Refuse(Diagnostic &diagnostic, Status status, std::string_view subject = {}) {
		diagnostic = {status, std::string(subject.substr(0, 128))};
		return status;
	}
	inline bool ValidName(std::string_view name) {
		return !name.empty() && name.size() <= 128 && name.find('\0') == std::string_view::npos;
	}
	inline bool Reads(Access access) {
		return access == Access::Read || access == Access::ReadWrite;
	}
	inline bool Writes(Access access) {
		return access == Access::Write || access == Access::ReadWrite;
	}
	inline bool ValidAccess(Access access) {
		return Reads(access) || Writes(access);
	}
	inline bool Allows(Access permission, Access request) {
		return ValidAccess(permission) && ValidAccess(request) && (!Reads(request) || Reads(permission)) &&
			   (!Writes(request) || Writes(permission));
	}
	inline bool ValidLayout(ElementType type, uint32_t stride) {
		return type == ElementType::EngineInstance ? stride > 0 : ElementStride(type) == stride && stride > 0;
	}
	template <typename Collection> uint32_t Find(const Collection &collection, std::string_view name) {
		for (size_t index = 0; index < collection.size(); ++index)
			if (collection[index].Name == name) return static_cast<uint32_t>(index);
		return UINT32_MAX;
	}
}
