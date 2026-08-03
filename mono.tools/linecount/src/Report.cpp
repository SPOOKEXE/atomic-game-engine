#include <algorithm>
#include <cstdio>
#include <linecount/Report.hpp>
#include <map>

namespace linecount {

	namespace {

		// `12345` reads as a different number at a glance depending on how many
		// digits the row above it had. Grouping is the cheapest way to make a
		// column of totals comparable without aligning it.
		std::string Grouped(size_t value) {
			std::string digits = std::to_string(value);
			std::string result;
			result.reserve(digits.size() + digits.size() / 3);

			// The first group is whatever is left over, so `1234567` breaks as
			// `1,234,567` rather than `123,456,7`. The `i >= leading` guard is
			// load-bearing rather than defensive: these are unsigned, and
			// `i - leading` on an early digit wraps to a number that is a
			// multiple of three often enough to put a comma inside `123`.
			const size_t leading = digits.size() % 3 == 0 ? 3 : digits.size() % 3;
			for (size_t i = 0; i < digits.size(); ++i) {
				if (i >= leading && (i - leading) % 3 == 0) {
					result += ',';
				}
				result += digits[i];
			}
			return result;
		}

		// One decimal, and an em dash when there is nothing to take a share of.
		// `0.0%` of nothing is a number, and a number invites the reader to work
		// out what it is a share of.
		std::string Share(size_t part, size_t whole) {
			if (whole == 0) {
				return "—";
			}
			char buffer[16];
			const double percent = 100.0 * static_cast<double>(part) / static_cast<double>(whole);
			std::snprintf(buffer, sizeof(buffer), "%.1f%%", percent);
			return buffer;
		}

		// The directory a file's row is grouped under: its first `depth`
		// segments. A file at the root of the walk has no directory to name, so
		// it goes under `.` rather than under an empty cell.
		std::string Group(const std::string &path, size_t depth) {
			const size_t lastSlash = path.rfind('/');
			// A depth of zero is "do not group", which is one row holding
			// everything rather than a row per file — the per-file table is
			// what `IncludeFiles` is for.
			if (lastSlash == std::string::npos || depth == 0) {
				return ".";
			}

			size_t cut = 0;
			for (size_t segment = 0; segment < depth; ++segment) {
				const size_t next = path.find('/', cut);
				// Past the last separator is past the directory part. A file
				// shallower than the requested depth is grouped by the whole
				// directory it is in rather than by a prefix of its name.
				if (next == std::string::npos || next >= lastSlash) {
					return path.substr(0, lastSlash);
				}
				cut = next + 1;
			}
			return path.substr(0, cut - 1);
		}

		struct Row {
			std::string Name;
			size_t Files = 0;
			Counts Lines;
		};

		void AppendCountsRow(
			std::string &out, const std::string &name, size_t files, const Counts &lines, const Counts &total
		) {
			out += "| " + name + " | " + Grouped(files) + " | " + Grouped(lines.Code) + " | " +
				   Grouped(lines.Comment) + " | " + Grouped(lines.Empty) + " | " + Grouped(lines.Total()) +
				   " | " + Share(lines.Total(), total.Total()) + " |\n";
		}
	}

	std::string Markdown(const std::vector<FileCounts> &files, const ReportOptions &options) {
		std::string out = "# Line count\n\n";

		if (files.empty()) {
			out += "No C or C++ sources were found.\n";
			return out;
		}

		Counts total;
		for (const FileCounts &file : files) {
			total += file.Lines;
		}

		out += Grouped(files.size()) + (files.size() == 1 ? " file" : " files") + ", " +
			   Grouped(total.Total()) + " lines.\n\n";

		out += "| Kind | Lines | Share |\n";
		out += "|---|---:|---:|\n";
		out += "| Code | " + Grouped(total.Code) + " | " + Share(total.Code, total.Total()) + " |\n";
		out += "| Comment | " + Grouped(total.Comment) + " | " + Share(total.Comment, total.Total()) + " |\n";
		out += "| Empty | " + Grouped(total.Empty) + " | " + Share(total.Empty, total.Total()) + " |\n";
		out += "| **Total** | **" + Grouped(total.Total()) + "** | |\n";

		// A map so that the grouping is a lookup rather than a search, and
		// ordered so that the sort below has a deterministic starting point
		// whatever order the walk handed the files over in.
		std::map<std::string, Row> groups;
		for (const FileCounts &file : files) {
			const std::string name = Group(file.Path, options.GroupDepth);
			Row &row = groups[name];
			row.Name = name;
			row.Files++;
			row.Lines += file.Lines;
		}

		std::vector<Row> rows;
		rows.reserve(groups.size());
		for (const auto &[name, row] : groups) {
			rows.push_back(row);
		}
		std::sort(rows.begin(), rows.end(), [](const Row &left, const Row &right) {
			if (left.Lines.Code != right.Lines.Code) {
				return left.Lines.Code > right.Lines.Code;
			}
			return left.Name < right.Name;
		});

		out += "\n## By directory\n\n";
		out += "| Directory | Files | Code | Comment | Empty | Total | Share |\n";
		out += "|---|---:|---:|---:|---:|---:|---:|\n";
		for (const Row &row : rows) {
			AppendCountsRow(out, "`" + row.Name + "`", row.Files, row.Lines, total);
		}
		AppendCountsRow(out, "**Total**", files.size(), total, total);

		if (!options.IncludeFiles) {
			return out;
		}

		std::vector<FileCounts> sorted = files;
		std::sort(sorted.begin(), sorted.end(), [](const FileCounts &left, const FileCounts &right) {
			if (left.Lines.Code != right.Lines.Code) {
				return left.Lines.Code > right.Lines.Code;
			}
			return left.Path < right.Path;
		});

		out += "\n## By file\n\n";
		out += "| File | Code | Comment | Empty | Total |\n";
		out += "|---|---:|---:|---:|---:|\n";
		for (const FileCounts &file : sorted) {
			out += "| `" + file.Path + "` | " + Grouped(file.Lines.Code) + " | " +
				   Grouped(file.Lines.Comment) + " | " + Grouped(file.Lines.Empty) + " | " +
				   Grouped(file.Lines.Total()) + " |\n";
		}

		return out;
	}
}
