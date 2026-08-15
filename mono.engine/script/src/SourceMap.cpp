#include "SourceMap.hpp"

#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

namespace engine::script {

	namespace {

		// Base64 for VLQ, which is the standard alphabet and not the URL one.
		int8_t Base64Digit(char character) {
			if (character >= 'A' && character <= 'Z') {
				return static_cast<int8_t>(character - 'A');
			}
			if (character >= 'a' && character <= 'z') {
				return static_cast<int8_t>(character - 'a' + 26);
			}
			if (character >= '0' && character <= '9') {
				return static_cast<int8_t>(character - '0' + 52);
			}
			if (character == '+') {
				return 62;
			}
			if (character == '/') {
				return 63;
			}
			return -1;
		}

		// One base64 VLQ field, advancing `at`.
		//
		// **The sign is the low bit of the *first* digit rather than of the
		// assembled number**, which is the part of the encoding that is easy to
		// get backwards - and getting it backwards produces plausible line
		// numbers rather than an error, because most deltas are small.
		//
		// @return `false` on a character outside the alphabet or a field that
		//         runs off the end, which fails the whole map.
		bool ReadVlq(std::string_view mappings, size_t &at, int32_t &value) {
			int32_t assembled = 0;
			int32_t shift = 0;
			bool more = true;

			while (more) {
				if (at >= mappings.size()) {
					return false;
				}
				const int8_t digit = Base64Digit(mappings[at]);
				if (digit < 0) {
					return false;
				}
				at++;

				more = (digit & 0x20) != 0;
				assembled += (digit & 0x1F) << shift;
				shift += 5;
			}

			const bool negative = (assembled & 1) != 0;
			assembled >>= 1;
			value = negative ? -assembled : assembled;
			return true;
		}

		// The `mappings` field, which is the whole of what this reader is for.
		//
		// Generated lines are separated by `;` and segments within a line by
		// `,`. A segment is one, four or five VLQ fields; the third is the
		// source line, delta-encoded **across the whole file** rather than reset
		// per line. So every segment has to be decoded even though only the
		// first of each line is kept - skipping the rest would desynchronise the
		// running total for every line after it.
		bool DecodeMappings(std::string_view mappings, std::vector<uint32_t> &lines) {
			int32_t sourceLine = 0;
			int32_t sourceIndex = 0;
			int32_t sourceColumn = 0;
			int32_t nameIndex = 0;

			size_t at = 0;
			uint32_t firstOfLine = 0;
			bool haveFirst = false;

			const auto endLine = [&]() {
				lines.push_back(haveFirst ? firstOfLine : 0u);
				haveFirst = false;
			};

			while (true) {
				if (at >= mappings.size()) {
					endLine();
					break;
				}

				const char character = mappings[at];
				if (character == ';') {
					at++;
					endLine();
					continue;
				}
				if (character == ',') {
					at++;
					continue;
				}

				// The generated column, which is read and discarded - see the
				// header on why no column is recovered.
				int32_t ignored = 0;
				if (!ReadVlq(mappings, at, ignored)) {
					return false;
				}

				// A one-field segment says "generated code with no origin".
				if (at < mappings.size() && (mappings[at] == ',' || mappings[at] == ';')) {
					continue;
				}
				if (at >= mappings.size()) {
					continue;
				}

				int32_t sourceDelta = 0;
				int32_t lineDelta = 0;
				int32_t columnDelta = 0;
				if (!ReadVlq(mappings, at, sourceDelta) || !ReadVlq(mappings, at, lineDelta) ||
					!ReadVlq(mappings, at, columnDelta)) {
					return false;
				}
				sourceIndex += sourceDelta;
				sourceLine += lineDelta;
				sourceColumn += columnDelta;

				// The optional fifth field, read only to stay aligned.
				if (at < mappings.size() && mappings[at] != ',' && mappings[at] != ';') {
					int32_t nameDelta = 0;
					if (!ReadVlq(mappings, at, nameDelta)) {
						return false;
					}
					nameIndex += nameDelta;
				}

				if (!haveFirst && sourceLine >= 0) {
					// The map counts source lines from zero and every reader of
					// a stack frame counts from one.
					firstOfLine = static_cast<uint32_t>(sourceLine) + 1u;
					haveFirst = true;
				}
			}

			(void)sourceIndex;
			(void)sourceColumn;
			(void)nameIndex;
			return true;
		}

	}

	uint32_t SourceMap::LineFor(uint32_t generated) const {
		if (generated == 0 || generated > SourceLines.size()) {
			return 0;
		}
		return SourceLines[generated - 1];
	}

	std::optional<SourceMap> LoadSourceMap(const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) {
			return std::nullopt;
		}

		// `nullptr` callback and `allow_exceptions == false`: a malformed map is
		// a `discarded` value rather than a throw, for the reason the header
		// gives - nothing upstream of here can act on it.
		const nlohmann::json document = nlohmann::json::parse(file, nullptr, false);
		if (document.is_discarded() || !document.is_object()) {
			return std::nullopt;
		}

		const auto mappings = document.find("mappings");
		if (mappings == document.end() || !mappings->is_string()) {
			return std::nullopt;
		}

		SourceMap map;
		if (!DecodeMappings(mappings->get<std::string>(), map.SourceLines)) {
			return std::nullopt;
		}

		// **The first source and only the first.** `tsc` emits one source per
		// map because it transpiles file by file under `--isolatedModules`, and
		// a bundler's many-source map would need the source index this reader
		// discards. Naming the wrong file is worse than naming none.
		if (const auto sources = document.find("sources"); sources != document.end() && sources->is_array() &&
														   sources->size() == 1 &&
														   sources->front().is_string()) {
			const auto named = sources->front().get<std::string>();

			// Resolved against the map's own directory, because that is what the
			// specification says the paths are relative to - and the maps this
			// engine reads sit in a stage directory whose relative path back to
			// the repository is meaningless to anyone reading a log.
			std::error_code failed;
			const std::filesystem::path resolved =
				std::filesystem::weakly_canonical(path.parent_path() / named, failed);
			map.Source = failed ? named : resolved.string();
		}

		return map;
	}

	std::string MapStackFrames(std::string_view text) {
		std::string out;
		out.reserve(text.size());

		size_t at = 0;
		while (at < text.size()) {
			// A frame is found by its extension rather than by matching the
			// whole of a VM's stack format, because the formats differ between
			// VMs and versions and the file name does not.
			const size_t suffix = text.find(".js:", at);
			if (suffix == std::string_view::npos) {
				out.append(text.substr(at));
				break;
			}

			// Back to the start of the path. A stack frame wraps the path in
			// `(` `)` and a bare message may not, so any of these ends it.
			size_t start = suffix;
			while (start > at) {
				const char character = text[start - 1];
				if (character == '(' || character == ' ' || character == '\n' || character == '\t') {
					break;
				}
				start--;
			}

			size_t digits = suffix + 4;
			uint32_t line = 0;
			while (digits < text.size() && (std::isdigit(static_cast<unsigned char>(text[digits])) != 0)) {
				line = line * 10 + static_cast<uint32_t>(text[digits] - '0');
				digits++;
			}

			// The column, when the VM printed one. **It is dropped on a rewrite
			// rather than carried**, because it is a column in the generated
			// file and pairing it with a source line produces `scene.ts:13:15`
			// pointing at a character that is somewhere else entirely - the
			// exact "plausible and wrong" this reader exists to avoid. Recovering
			// the real column is a per-segment search this does not do.
			size_t column = digits;
			if (column < text.size() && text[column] == ':') {
				size_t scan = column + 1;
				while (scan < text.size() && (std::isdigit(static_cast<unsigned char>(text[scan])) != 0)) {
					scan++;
				}
				if (scan > column + 1) {
					column = scan;
				}
			}

			const std::string_view file = text.substr(start, suffix + 3 - start);
			out.append(text.substr(at, start - at));

			const std::optional<SourceMap> map =
				line == 0 ? std::nullopt : LoadSourceMap(std::filesystem::path(std::string(file) + ".map"));
			const uint32_t mapped = map.has_value() ? map->LineFor(line) : 0;

			if (mapped != 0 && !map->Source.empty()) {
				out.append(map->Source);
				out.push_back(':');
				out.append(std::to_string(mapped));
				at = column;
			} else {
				// Untouched, column included: this frame is being left exactly
				// as the VM printed it.
				out.append(text.substr(start, digits - start));
				at = digits;
			}
		}

		return out;
	}

}
