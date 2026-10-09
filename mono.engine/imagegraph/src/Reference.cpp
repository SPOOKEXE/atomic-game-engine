#include <engine/imagegraph/Reference.hpp>

#include <charconv>
#include <cstdint>

namespace engine::imagegraph {
	bool IsReferenceToken(std::string_view value) noexcept {
		if (value.empty() || value.size() > 128 || value == "." || value == "..") return false;
		for (const unsigned char c : value)
			if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
				  c == '-' || c == '.'))
				return false;
		return true;
	}
	static bool PortableAsset(std::string_view value) noexcept {
		if (value.empty() || value.size() > 4096 || value.front() == '/') return false;
		for (const unsigned char c : value)
			if (c <= 32 || c >= 127 || c == '\\' || c == ':' || c == '#' || c == '?' || c == '%')
				return false;
		while (!value.empty()) {
			const auto slash = value.find('/');
			const auto part = value.substr(0, slash);
			if (part.empty() || part == "." || part == "..") return false;
			if (slash == std::string_view::npos) return true;
			value.remove_prefix(slash + 1);
			if (value.empty()) return false;
		}
		return false;
	}
	bool IsEditableImageReference(std::string_view value) noexcept {
		constexpr std::string_view prefix = "editable-image://";
		if (!value.starts_with(prefix)) return false;
		value.remove_prefix(prefix.size());
		if (value.empty() || value.size() > 20 || (value.size() > 1 && value.front() == '0')) return false;
		uint64_t id = 0;
		const auto parsed = std::from_chars(value.data(), value.data() + value.size(), id);
		return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() && id != 0;
	}
	bool IsRuntimeTexture(std::string_view value) noexcept {
		return value.ends_with(".atex") && PortableAsset(value);
	}
	bool IsRuntimeAsset(std::string_view value) noexcept {
		return value.ends_with(".aimagegraph") && PortableAsset(value);
	}
	bool IsReference(std::string_view value) noexcept {
		return value.starts_with("imagegraph://") || value.starts_with("imagegraph-instance://");
	}
	bool ParseReference(std::string_view value, Reference &out) {
		Reference candidate;
		if (value.starts_with("imagegraph-instance://")) {
			candidate.Kind = ReferenceKind::Instance;
			value.remove_prefix(22);
		} else if (value.starts_with("imagegraph://"))
			value.remove_prefix(13);
		else
			return false;
		const auto hash = value.find('#');
		if (hash == std::string_view::npos || !IsReferenceToken(value.substr(hash + 1))) return false;
		const auto name = value.substr(0, hash);
		if (candidate.Kind == ReferenceKind::Asset ? !IsRuntimeAsset(name) : !IsReferenceToken(name))
			return false;
		candidate.Name = name;
		candidate.Output = value.substr(hash + 1);
		out = std::move(candidate);
		return true;
	}
}
