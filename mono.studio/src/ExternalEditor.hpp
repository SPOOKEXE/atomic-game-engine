#pragma once

// Private file bridge behind Studio's external source-editor integration.

#include <studio/Editor.hpp>

namespace studio {
	const char *Describe(ExternalEditorKind kind);
	std::optional<ExternalEditorKind> ExternalEditorKindOf(std::string_view text);

	std::filesystem::path ExternalDocumentPath(
		const std::filesystem::path &root,
		std::string_view world,
		uint64_t instance,
		std::string_view name,
		std::string_view extension
	);

	bool StageExternalDocument(
		const std::filesystem::path &path,
		std::string_view text,
		ExternalDocument &document,
		std::string &error
	);

	enum class ExternalRefresh { Unchanged, Reloaded, Conflict, Failed };

	ExternalRefresh RefreshExternalDocument(
		ExternalDocument &document, std::string_view current, std::string &reloaded, std::string &error
	);

	bool AcceptExternalDocument(ExternalDocument &document, std::string &text, std::string &error);
	bool KeepStudioDocument(ExternalDocument &document, std::string_view text, std::string &error);
	bool LaunchExternalEditor(
		const ExternalEditorSettings &settings, const std::filesystem::path &path, std::string &error
	);
}
