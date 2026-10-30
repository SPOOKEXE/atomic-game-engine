#include "ExternalEditor.hpp"

#include <SDL3/SDL_error.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_process.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

namespace studio {
	namespace {
		bool
		WriteDocument(const std::filesystem::path &path, const std::string_view text, std::string &error) {
			std::error_code failure;
			std::filesystem::create_directories(path.parent_path(), failure);
			if (failure) {
				error = "could not create " + path.parent_path().string() + ": " + failure.message();
				return false;
			}

			std::ofstream output(path, std::ios::binary | std::ios::trunc);
			if (!output) {
				error = "could not write " + path.string();
				return false;
			}
			output.write(text.data(), static_cast<std::streamsize>(text.size()));
			if (!output) {
				error = "could not finish writing " + path.string();
				return false;
			}
			return true;
		}

		bool ReadDocument(const std::filesystem::path &path, std::string &text, std::string &error) {
			std::ifstream input(path, std::ios::binary);
			if (!input) {
				error = "could not read " + path.string();
				return false;
			}
			std::ostringstream buffer;
			buffer << input.rdbuf();
			if (!input.good() && !input.eof()) {
				error = "could not finish reading " + path.string();
				return false;
			}
			text = buffer.str();
			return true;
		}

		std::string SafePart(const std::string_view text, const std::string_view fallback) {
			std::string safe;
			safe.reserve(std::min<size_t>(text.size(), 64));
			for (const unsigned char character : text) {
				if (safe.size() >= 64) {
					break;
				}
				safe.push_back(
					std::isalnum(character) || character == '-' || character == '_'
						? static_cast<char>(character)
						: '_'
				);
			}
			return safe.empty() ? std::string(fallback) : safe;
		}

		std::string FileUrl(const std::filesystem::path &path) {
			const std::string source = std::filesystem::absolute(path).generic_string();
			constexpr std::array<char, 16> HEX{
				'0',
				'1',
				'2',
				'3',
				'4',
				'5',
				'6',
				'7',
				'8',
				'9',
				'A',
				'B',
				'C',
				'D',
				'E',
				'F',
			};
			std::string url = source.starts_with('/') ? "file://" : "file:///";
			for (const unsigned char character : source) {
				if (std::isalnum(character) || character == '/' || character == ':' || character == '-' ||
					character == '_' || character == '.') {
					url.push_back(static_cast<char>(character));
					continue;
				}
				url.push_back('%');
				url.push_back(HEX[character >> 4u]);
				url.push_back(HEX[character & 0x0fu]);
			}
			return url;
		}
	}

	const char *Describe(const ExternalEditorKind kind) {
		switch (kind) {
		case ExternalEditorKind::System:
			return "System default";
		case ExternalEditorKind::VisualStudioCode:
			return "Visual Studio Code";
		case ExternalEditorKind::Notepad:
			return "Notepad";
		case ExternalEditorKind::Custom:
			return "Custom";
		}
		return "System default";
	}

	std::optional<ExternalEditorKind> ExternalEditorKindOf(const std::string_view text) {
		for (size_t index = 0; index < EXTERNAL_EDITOR_KIND_COUNT; index++) {
			const auto candidate = static_cast<ExternalEditorKind>(index);
			if (text == Describe(candidate)) {
				return candidate;
			}
		}
		return std::nullopt;
	}

	std::filesystem::path ExternalDocumentPath(
		const std::filesystem::path &root,
		const std::string_view world,
		const uint64_t instance,
		const std::string_view name,
		const std::string_view extension
	) {
		const std::string leaf =
			std::to_string(instance) + "-" + SafePart(name, "Source") + "." + SafePart(extension, "txt");
		return root / SafePart(world, "World") / leaf;
	}

	bool StageExternalDocument(
		const std::filesystem::path &path,
		const std::string_view text,
		ExternalDocument &document,
		std::string &error
	) {
		error.clear();
		if (path.empty()) {
			error = "external document path is empty";
			return false;
		}

		std::filesystem::path staged = path;
		std::filesystem::path reservation;
		if (!document.Active() || path != document.Path) {
			static std::atomic<uint64_t> nextSlot{0};
			std::error_code failure;
			std::filesystem::create_directories(path.parent_path(), failure);
			if (failure) {
				error = "could not create " + path.parent_path().string() + ": " + failure.message();
				return false;
			}
			// Reserve a directory atomically so different tabs and Studio processes
			// cannot truncate another editor's draft, even when names collide.
			for (uint64_t attempts = 0;; ++attempts) {
				const uint64_t index = nextSlot.fetch_add(1, std::memory_order_relaxed);
				reservation = path.parent_path() / ("document-" + std::to_string(index));
				if (std::filesystem::create_directory(reservation, failure)) break;
				if (failure && failure != std::errc::file_exists) {
					error = "could not reserve " + reservation.string() + ": " + failure.message();
					return false;
				}
				if (attempts == std::numeric_limits<uint64_t>::max()) {
					error = "no external document slots are available";
					return false;
				}
				failure.clear();
			}
			staged = reservation / path.filename();
		}

		if (!WriteDocument(staged, text, error)) {
			if (!reservation.empty()) {
				std::error_code ignored;
				std::filesystem::remove_all(reservation, ignored);
			}
			return false;
		}
		std::error_code failure;
		const auto written = std::filesystem::last_write_time(staged, failure);
		if (failure) {
			error = "could not inspect " + staged.string() + ": " + failure.message();
			if (!reservation.empty()) {
				std::error_code ignored;
				std::filesystem::remove_all(reservation, ignored);
			}
			return false;
		}
		document.Path = staged;
		document.Baseline.assign(text);
		document.Written = written;
		document.Conflict = false;
		return true;
	}

	ExternalRefresh RefreshExternalDocument(
		ExternalDocument &document, const std::string_view current, std::string &reloaded, std::string &error
	) {
		error.clear();
		reloaded.clear();
		if (!document.Active()) {
			return ExternalRefresh::Unchanged;
		}

		std::error_code failure;
		const auto written = std::filesystem::last_write_time(document.Path, failure);
		if (failure) {
			error = "could not inspect " + document.Path.string() + ": " + failure.message();
			return ExternalRefresh::Failed;
		}
		if (written == document.Written) {
			return document.Conflict ? ExternalRefresh::Conflict : ExternalRefresh::Unchanged;
		}

		std::string external;
		if (!ReadDocument(document.Path, external, error)) {
			return ExternalRefresh::Failed;
		}
		document.Written = written;
		if (external == document.Baseline) {
			return document.Conflict ? ExternalRefresh::Conflict : ExternalRefresh::Unchanged;
		}
		if (current != document.Baseline && current != external) {
			document.Conflict = true;
			return ExternalRefresh::Conflict;
		}

		document.Baseline = external;
		document.Conflict = false;
		reloaded = std::move(external);
		return ExternalRefresh::Reloaded;
	}

	bool AcceptExternalDocument(ExternalDocument &document, std::string &text, std::string &error) {
		std::string external;
		if (!document.Active() || !ReadDocument(document.Path, external, error)) {
			return false;
		}
		std::error_code failure;
		const auto written = std::filesystem::last_write_time(document.Path, failure);
		if (failure) {
			error = "could not inspect " + document.Path.string() + ": " + failure.message();
			return false;
		}
		text = external;
		document.Baseline = std::move(external);
		document.Written = written;
		document.Conflict = false;
		return true;
	}

	bool KeepStudioDocument(ExternalDocument &document, const std::string_view text, std::string &error) {
		return document.Active() && StageExternalDocument(document.Path, text, document, error);
	}

	bool LaunchExternalEditor(
		const ExternalEditorSettings &settings, const std::filesystem::path &path, std::string &error
	) {
		error.clear();
		if (settings.Kind == ExternalEditorKind::System) {
			if (SDL_OpenURL(FileUrl(path).c_str())) {
				return true;
			}
			error = SDL_GetError();
			return false;
		}

		std::string executable = settings.Executable;
		if (executable.empty()) {
			if (settings.Kind == ExternalEditorKind::Custom) {
				error = "the custom external editor has no executable";
				return false;
			}
			executable = settings.Kind == ExternalEditorKind::VisualStudioCode ? "code" : "notepad";
		}
		std::vector<std::string> owned{executable};
		if (settings.Kind == ExternalEditorKind::VisualStudioCode) {
			owned.emplace_back("--reuse-window");
		}
		owned.push_back(path.string());

		std::vector<const char *> arguments;
		arguments.reserve(owned.size() + 1);
		for (const std::string &argument : owned) {
			arguments.push_back(argument.c_str());
		}
		arguments.push_back(nullptr);

		SDL_Process *process = SDL_CreateProcess(arguments.data(), false);
		if (process == nullptr) {
			error = SDL_GetError();
			return false;
		}
		// Destroying the SDL handle does not stop the external editor. Studio
		// watches the staged file and does not own the application's lifetime.
		SDL_DestroyProcess(process);
		return true;
	}
}
