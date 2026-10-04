#include "FontPath.hpp"

#include <algorithm>

namespace engine::imagegraph::detail {
	Status ResolveSourceFontPath(
		std::string_view authored,
		const SourceFontContext *context,
		uint64_t maximumBytes,
		std::string &output,
		std::string &failure
	) {
		const auto fail = [&](Status status, std::string message) {
			failure = std::move(message);
			return status;
		};
		if (authored.size() > Limits::MaximumTextBytes)
			return fail(Status::LimitExceeded, "font path exceeds source text bound");
		if (!context || !context->AliasMapKnown)
			return fail(Status::UnsupportedExecution, "font getter requires an observed source alias map");
		const auto contextBytes = SourceFontContextRetainedBytes(*context);
		if (!contextBytes) return fail(Status::InvalidValue, "font namespace is malformed");
		constexpr uint64_t aliasWork = 16 * 1024 * 1024;
		if (!context->Aliases.empty() && authored.size() > aliasWork / context->Aliases.size())
			return fail(Status::LimitExceeded, "font alias comparison exceeds work bound");
		std::string_view selected = authored;
		bool alias = false;
		for (const auto &[name, path] : context->Aliases)
			if (name == authored) {
				selected = path;
				alias = true;
			}
		if (alias) {
			if (selected.size() > maximumBytes || std::string{}.capacity() > maximumBytes)
				return fail(Status::LimitExceeded, "font alias output exceeds copy budget");
			std::string candidate(selected);
			output = std::move(candidate);
			failure.clear();
			return Status::Ok;
		}
		// Source applies replacements sequentially, so a captured prefix can contain the next token.
		const auto replace = [&](std::string_view source,
								 std::string_view token,
								 const std::optional<std::string> &replacement,
								 std::string &result) -> Status {
			if (source.find(token) == std::string_view::npos) {
				result.assign(source);
				return Status::Ok;
			}
			if (!replacement) return fail(Status::UnsupportedExecution, "font path prefix is unobserved");
			size_t size = 0, offset = 0;
			while (offset < source.size()) {
				const auto at = source.find(token, offset);
				const auto end = at == std::string_view::npos ? source.size() : at;
				if (end - offset > Limits::MaximumTextBytes - size)
					return fail(Status::LimitExceeded, "expanded font path exceeds source text bound");
				size += end - offset;
				if (at == std::string_view::npos) break;
				if (replacement->size() > Limits::MaximumTextBytes - size)
					return fail(Status::LimitExceeded, "expanded font path exceeds source text bound");
				size += replacement->size();
				offset = at + token.size();
			}
			if (size > maximumBytes / 3 || std::string{}.capacity() > maximumBytes / 3)
				return fail(Status::LimitExceeded, "font path replacement exceeds workspace budget");
			std::string candidate(size, '\0');
			size_t write = 0;
			offset = 0;
			while (offset < source.size()) {
				const auto at = source.find(token, offset);
				const auto end = at == std::string_view::npos ? source.size() : at;
				std::copy(source.begin() + offset, source.begin() + end, candidate.begin() + write);
				write += end - offset;
				if (at == std::string_view::npos) break;
				std::copy(replacement->begin(), replacement->end(), candidate.begin() + write);
				write += replacement->size();
				offset = at + token.size();
			}
			result = std::move(candidate);
			return Status::Ok;
		};
		if (selected.size() > maximumBytes / 3 || std::string{}.capacity() > maximumBytes / 3)
			return fail(Status::LimitExceeded, "font path exceeds workspace budget");
		std::string directoryExpanded, candidate;
		const auto directory = replace(selected, "%DIR%/", context->Directory, directoryExpanded);
		if (directory != Status::Ok) return directory;
		const auto application =
			replace(directoryExpanded, "%APP%/", context->ApplicationLocation, candidate);
		if (application != Status::Ok) return application;
		std::replace(candidate.begin(), candidate.end(), '\\', '/');
		if (candidate.starts_with("./") || candidate.starts_with("../")) {
			if (!context->ProjectPath)
				return fail(
					Status::UnsupportedExecution, "relative font path requires an observed project path"
				);
			if (!context->ProjectPath->empty()) {
				const auto parent = [](std::string_view path) {
					const auto slash = path.find_last_of("/\\");
					// Inspected HTML5 filename_dir retains paths with no separator or one at index zero.
					return slash == std::string_view::npos || slash == 0 ? path : path.substr(0, slash);
				};
				std::string_view dir = parent(*context->ProjectPath), tail = candidate;
				const bool combine = tail.starts_with("../");
				if (tail.starts_with("./"))
					tail.remove_prefix(2);
				else
					while (tail.starts_with("../")) {
						dir = parent(dir);
						tail.remove_prefix(3);
					}
				if (combine) {
					while (!dir.empty() && (dir.back() == '/' || dir.back() == '\\'))
						dir.remove_suffix(1);
					while (!tail.empty() && tail.front() == '/')
						tail.remove_prefix(1);
				}
				const uint64_t size = dir.size() + 1 + tail.size();
				if (size > Limits::MaximumTextBytes || size > maximumBytes / 3)
					return fail(Status::LimitExceeded, "relative font path exceeds copy budget");
				std::string joined(static_cast<size_t>(size), '\0');
				std::copy(dir.begin(), dir.end(), joined.begin());
				if (combine) std::replace(joined.begin(), joined.begin() + dir.size(), '\\', '/');
				joined[dir.size()] = '/';
				std::copy(tail.begin(), tail.end(), joined.begin() + dir.size() + 1);
				candidate = std::move(joined);
			}
		}
		output = std::move(candidate);
		failure.clear();
		return Status::Ok;
	}
}
