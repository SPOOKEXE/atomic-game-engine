#include <algorithm>
#include <fstream>
#include <sstream>
#include <testrunner/Report.hpp>

namespace testrunner {

	namespace fs = std::filesystem;

	namespace {

		// Written by the `mono` reporter in mono.build/testmain. The two are one
		// format with two halves; change either and change the other.
		constexpr std::string_view REPORT_HEADER = "# mono test report v2";

		std::vector<std::string_view> Fields(std::string_view line) {
			std::vector<std::string_view> fields;
			size_t start = 0;
			for (;;) {
				const size_t found = line.find('\t', start);
				if (found == std::string_view::npos) {
					fields.push_back(line.substr(start));
					return fields;
				}
				fields.push_back(line.substr(start, found - start));
				start = found + 1;
			}
		}

		// Anything that is not a number reads as zero rather than throwing. The
		// caller already has a stronger signal - the totals line, and the exit
		// status behind it - so a malformed field is not worth an exception.
		unsigned long long Number(std::string_view text) {
			unsigned long long value = 0;
			for (const char character : text) {
				if (character < '0' || character > '9') {
					return 0;
				}
				value = value * 10 + static_cast<unsigned long long>(character - '0');
			}
			return value;
		}

		// The first `depth` dot-separated components of an identifier, joined
		// back up. `Section("engine.ecs.column.core", 2)` is "engine.ecs".
		//
		// Shorter identifiers are their own section rather than being padded,
		// so a two-component suite still lands somewhere a person can find.
		std::string Section(std::string_view id, size_t depth) {
			size_t position = 0;
			for (size_t component = 0; component < depth; component++) {
				const size_t dot = id.find('.', position);
				if (dot == std::string_view::npos) {
					return std::string(id);
				}
				position = dot + 1;
			}
			return std::string(id.substr(0, position - 1));
		}

		// Every suite, in identifier order, so that the two documents agree with
		// each other and successive runs agree with themselves.
		std::vector<const SuiteReport *> Ordered(const std::vector<SuiteReport> &suites) {
			std::vector<const SuiteReport *> ordered;
			ordered.reserve(suites.size());
			for (const auto &suite : suites) {
				ordered.push_back(&suite);
			}
			std::sort(ordered.begin(), ordered.end(), [](const SuiteReport *left, const SuiteReport *right) {
				return left->Id < right->Id;
			});
			return ordered;
		}

		// The numbers a heading carries. Adding up the rows underneath it rather
		// than being tracked alongside them, because a total that is stored
		// separately from what it totals is a total that eventually disagrees.
		struct Tally {
			unsigned Suites = 0;
			unsigned SuitesGreen = 0;
			unsigned SuitesRan = 0;
			unsigned Cases = 0;
			unsigned Passed = 0;
			unsigned Failed = 0;
			unsigned Skipped = 0;
			unsigned Assertions = 0;
			unsigned long long Microseconds = 0;

			void Add(const SuiteReport &suite) {
				Suites++;
				SuitesGreen += suite.Passed ? 1 : 0;
				SuitesRan += suite.Ran ? 1 : 0;
				Cases += suite.CaseCount();
				Passed += suite.CasesPassed;
				Failed += suite.CasesFailed;
				Skipped += suite.CasesSkipped;
				Assertions += suite.AssertionsPassed + suite.AssertionsFailed;
				Microseconds += suite.Microseconds;
			}

			bool Green() const {
				return SuitesGreen == Suites;
			}

			// Whole per cent, rounded down, and never 100 unless it really is -
			// a report that rounds 999/1000 up to "100%" is a report that hides
			// the one thing worth reading.
			unsigned Percent() const {
				if (Cases == 0) {
					return 0;
				}
				return static_cast<unsigned>((static_cast<unsigned long long>(Passed) * 100) / Cases);
			}
		};

		Tally TallyOf(const std::vector<const SuiteReport *> &suites) {
			Tally tally;
			for (const auto *suite : suites) {
				tally.Add(*suite);
			}
			return tally;
		}

		// The sections of one level, in order, each holding the suites beneath
		// it. Built by walking an already-ordered list, so a section's suites
		// are contiguous and no map is needed to keep the order.
		struct Group {
			std::string Name;
			std::vector<const SuiteReport *> Suites;
		};

		std::vector<Group> GroupBy(const std::vector<const SuiteReport *> &suites, size_t depth) {
			std::vector<Group> groups;
			for (const auto *suite : suites) {
				auto name = Section(suite->Id, depth);
				if (groups.empty() || groups.back().Name != name) {
					groups.push_back(Group{std::move(name), {}});
				}
				groups.back().Suites.push_back(suite);
			}
			return groups;
		}

		// Quotes included, because the flamegraph puts a test name in a title
		// attribute and not only in an element. A name is prose somebody wrote;
		// the one with an apostrophe in it exists.
		std::string Escape(std::string_view text) {
			std::string escaped;
			escaped.reserve(text.size());
			for (const char character : text) {
				switch (character) {
				case '&':
					escaped += "&amp;";
					break;
				case '<':
					escaped += "&lt;";
					break;
				case '>':
					escaped += "&gt;";
					break;
				case '"':
					escaped += "&quot;";
					break;
				case '\'':
					escaped += "&#39;";
					break;
				default:
					escaped += character;
				}
			}
			return escaped;
		}

		// "pass", "pass (cached)", "FAIL". The parenthesis is the whole point of
		// the column: a suite the cascade skipped is being reported on the
		// strength of a previous run, and the row says so.
		std::string Outcome(const SuiteReport &suite) {
			if (!suite.Passed) {
				return "FAIL";
			}
			return suite.Ran ? "pass" : "pass (cached)";
		}

		// The most expensive single case in a suite.
		//
		// Next to the suite's own time it is the whole diagnosis: a slow suite
		// whose slowest case is most of it is one pathological test, and a slow
		// suite whose slowest case is a fraction of it is two hundred ordinary
		// ones. Those want different fixes.
		//
		// An em dash for a cached suite, whose per-case detail smart-tests.txt
		// does not keep. Zero would be a claim, and a false one.
		std::string Slowest(const SuiteReport &suite) {
			if (suite.Cases.empty()) {
				return "-";
			}

			unsigned long long worst = 0;
			for (const auto &entry : suite.Cases) {
				worst = std::max(worst, entry.Microseconds);
			}
			return FormatDuration(worst);
		}

		std::string Sentence(const Tally &tally) {
			std::ostringstream text;
			text << tally.Passed << " of " << tally.Cases << " case(s) passed";
			if (tally.Failed > 0) {
				text << ", " << tally.Failed << " failed";
			}
			if (tally.Skipped > 0) {
				text << ", " << tally.Skipped << " skipped";
			}
			text << " across " << tally.Suites << " suite(s) in " << FormatDuration(tally.Microseconds)
				 << '.';
			return text.str();
		}
	}

	std::string FormatDuration(unsigned long long microseconds) {
		std::ostringstream text;
		text.setf(std::ios::fixed, std::ios::floatfield);

		const auto value = static_cast<double>(microseconds);
		if (microseconds >= 1'000'000) {
			text.precision(2);
			text << value / 1'000'000.0 << " s";
		} else if (microseconds >= 10'000) {
			text.precision(0);
			text << value / 1'000.0 << " ms";
		} else if (microseconds >= 1'000) {
			text.precision(1);
			text << value / 1'000.0 << " ms";
		} else {
			// Microseconds spelled with the sign rather than as "us". The
			// documents are UTF-8 and say so in a meta tag; a report that
			// spells its own units defensively is a report that has decided
			// its reader cannot be trusted with a character.
			text << microseconds << " \xc2\xb5s";
		}

		return text.str();
	}

	namespace {

		// One box of the flamegraph. Width is time, depth is the path through
		// the identifiers, and the bottom row is individual test cases.
		//
		// A flamegraph rather than a bar chart of the slowest suites, because
		// the question "what is the test run spending its time on" is asked at
		// whichever level the answer happens to live at. A module can be slow
		// because one case is pathological or because two hundred of them are
		// ordinary, and those want different fixes - a chart that has already
		// picked a level cannot tell them apart.
		struct Frame {
			// What the box is: an identifier, or a test case's name.
			std::string Label;

			// Everything worth knowing about it that is not its name - what it
			// cost, what it holds, whether it is red. Kept apart from the label
			// so the hover readout can set the two in different weights.
			std::string Detail;

			unsigned long long Microseconds = 0;
			bool Failed = false;
			bool Cached = false;
			std::vector<Frame> Children;
		};

		// Middle dot with spaces. The readout is a run-on line of small facts
		// and a comma between them reads as one sentence, which it is not.
		constexpr std::string_view SEPARATOR = " \xc2\xb7 ";

		// Widest first, so the thing worth looking at is where the eye already
		// is. Ties break on the label, so the document stays reproducible.
		void SortByCost(std::vector<Frame> &frames) {
			std::sort(frames.begin(), frames.end(), [](const Frame &left, const Frame &right) {
				if (left.Microseconds != right.Microseconds) {
					return left.Microseconds > right.Microseconds;
				}
				return left.Label < right.Label;
			});
		}

		// What a section's box says when hovered. A section is not a thing that
		// ran, so what it has to report is what is underneath it.
		std::string SectionDetail(const Tally &tally) {
			std::ostringstream detail;
			detail << FormatDuration(tally.Microseconds) << SEPARATOR << tally.Suites << " suite(s)"
				   << SEPARATOR << tally.Cases << " case(s)";
			if (tally.Failed > 0) {
				detail << ", " << tally.Failed << " failing";
			}
			detail << SEPARATOR << tally.Assertions << " assertion(s)";
			if (tally.SuitesRan < tally.Suites) {
				detail << SEPARATOR << (tally.Suites - tally.SuitesRan) << " carried forward from the cache";
			}
			return detail.str();
		}

		Frame FrameOfSuite(const SuiteReport &suite) {
			Frame frame;
			frame.Label = suite.Id;
			frame.Microseconds = suite.Microseconds;
			frame.Failed = !suite.Passed;
			frame.Cached = !suite.Ran;

			std::ostringstream detail;
			detail << FormatDuration(suite.Microseconds) << SEPARATOR << suite.CaseCount() << " case(s)";
			if (suite.CasesFailed > 0) {
				detail << ", " << suite.CasesFailed << " failing";
			}
			if (suite.CasesSkipped > 0) {
				detail << ", " << suite.CasesSkipped << " skipped";
			}
			detail << SEPARATOR << (suite.AssertionsPassed + suite.AssertionsFailed) << " assertion(s)";

			if (!suite.Cases.empty()) {
				// Clamped. The two numbers come from two clocks in two
				// processes, and an unsigned subtraction that goes negative
				// does not report a small inconsistency - it reports half a
				// million years.
				const auto inside = suite.CaseMicroseconds();
				const auto outside = suite.Microseconds > inside ? suite.Microseconds - inside : 0;

				detail << SEPARATOR << "slowest case " << Slowest(suite) << SEPARATOR
					   << FormatDuration(outside) << " outside any case";
			} else {
				detail << SEPARATOR << "carried forward from the cache, so no per-case breakdown";
			}
			frame.Detail = detail.str();

			for (const auto &entry : suite.Cases) {
				Frame child;
				child.Label = entry.Name;
				child.Microseconds = entry.Microseconds;
				child.Failed = entry.Failed;

				std::ostringstream childDetail;
				childDetail << FormatDuration(entry.Microseconds) << SEPARATOR << "in " << suite.Id;
				if (entry.Failed) {
					childDetail << SEPARATOR << "FAILED";
				} else if (entry.Skipped) {
					childDetail << SEPARATOR << "skipped";
				}
				child.Detail = childDetail.str();

				frame.Children.push_back(std::move(child));
			}

			SortByCost(frame.Children);
			return frame;
		}

		// The whole tree: everything -> A -> A.B -> suite -> case.
		//
		// A section's width is the sum of its suites rather than a measurement
		// of its own, because there is no such thing as running a section.
		Frame BuildFlamegraph(const std::vector<const SuiteReport *> &suites) {
			Frame root;
			root.Label = "all suites";

			for (const auto &top : GroupBy(suites, 1)) {
				Frame first;
				first.Label = top.Name;

				for (const auto &middle : GroupBy(top.Suites, 2)) {
					Frame second;
					second.Label = middle.Name;

					for (const auto *suite : middle.Suites) {
						second.Children.push_back(FrameOfSuite(*suite));
						second.Microseconds += suite->Microseconds;
						second.Failed = second.Failed || !suite->Passed;
					}

					SortByCost(second.Children);
					second.Detail = SectionDetail(TallyOf(middle.Suites));
					first.Microseconds += second.Microseconds;
					first.Failed = first.Failed || second.Failed;
					first.Children.push_back(std::move(second));
				}

				SortByCost(first.Children);
				first.Detail = SectionDetail(TallyOf(top.Suites));
				root.Microseconds += first.Microseconds;
				root.Failed = root.Failed || first.Failed;
				root.Children.push_back(std::move(first));
			}

			SortByCost(root.Children);
			root.Detail = SectionDetail(TallyOf(suites));
			return root;
		}

		// A hue per label, so that a box keeps its colour between runs and a
		// person can follow one across two reports. Warm end of the wheel only,
		// which is what makes a flamegraph read as one.
		unsigned Hue(std::string_view label) {
			// FNV-1a. Anything stable would do; this one is four lines.
			unsigned long long hash = 14695981039346656037ULL;
			for (const char character : label) {
				hash ^= static_cast<unsigned char>(character);
				hash *= 1099511628211ULL;
			}
			return 15 + static_cast<unsigned>(hash % 40);
		}

		// @param width  This frame's share of its parent, as a percentage. It is
		//               also the box's CSS width, which is the only thing a
		//               flamegraph means.
		// @param parent The enclosing frame's label, so the readout can say what
		//               the share is a share of. Empty at the root.
		void
		RenderFrame(std::ostringstream &page, const Frame &frame, double width, const std::string &parent) {
			// Below this a box is thinner than its own border and reads as a
			// line of noise. Its time is still counted by every ancestor.
			constexpr double VISIBLE_PERCENT = 0.12;
			if (width < VISIBLE_PERCENT) {
				return;
			}

			std::ostringstream colour;
			if (frame.Failed) {
				colour << "hsl(2 72% 52%)";
			} else if (frame.Cached) {
				colour << "hsl(" << Hue(frame.Label) << " 22% 58%)";
			} else {
				colour << "hsl(" << Hue(frame.Label) << " 72% 58%)";
			}

			std::ostringstream detail;
			detail << frame.Detail;
			if (!parent.empty()) {
				detail.setf(std::ios::fixed, std::ios::floatfield);
				detail.precision(1);
				detail << SEPARATOR << width << "% of " << parent;
			}

			page.setf(std::ios::fixed, std::ios::floatfield);
			page.precision(4);
			page << "<div class=\"frame\" style=\"width:" << width << "%\">";

			// title as well as the readout. The readout is the one a person
			// sees; the attribute is what a screen reader announces and what
			// survives the page being saved somewhere its CSS is not.
			page << "<div class=\"box\" style=\"background:" << colour.str() << "\" title=\""
				 << Escape(frame.Label + " - " + detail.str()) << "\"><span>" << Escape(frame.Label)
				 << "</span></div>";

			// A sibling of the box rather than a child of it: the box clips its
			// own label, and anything inside it would be clipped too.
			page << "<div class=\"tip\"><b>" << Escape(frame.Label) << "</b><span>" << Escape(detail.str())
				 << "</span></div>";

			if (!frame.Children.empty() && frame.Microseconds > 0) {
				page << "<div class=\"row\">";
				for (const auto &child : frame.Children) {
					RenderFrame(
						page,
						child,
						100.0 * static_cast<double>(child.Microseconds) /
							static_cast<double>(frame.Microseconds),
						frame.Label
					);
				}
				page << "</div>";
			}

			page << "</div>";
		}
	}

	// -----------------------------------------------------------------------
	// Reading what a test binary reported
	// -----------------------------------------------------------------------

	bool ParseSuiteReport(std::string_view text, SuiteReport &report) {
		std::string line;
		std::istringstream lines{std::string(text)};

		if (!std::getline(lines, line) || line != REPORT_HEADER) {
			return false;
		}

		bool totalled = false;
		while (std::getline(lines, line)) {
			if (line.empty()) {
				continue;
			}

			const auto fields = Fields(line);
			if (fields[0] == "case" && fields.size() >= 6) {
				CaseReport entry;
				entry.Failed = fields[1] == "fail";
				entry.Skipped = fields[1] == "skip";
				entry.Microseconds = Number(fields[4]);
				// The name is the last field and may hold anything but a tab,
				// so it is taken as the remainder rather than as one field.
				entry.Name = line.substr(line.size() - fields.back().size());

				report.Cases.push_back(std::move(entry));
				continue;
			}

			if (fields[0] == "total" && fields.size() >= 6) {
				report.CasesPassed = static_cast<unsigned>(Number(fields[1]));
				report.CasesFailed = static_cast<unsigned>(Number(fields[2]));
				report.CasesSkipped = static_cast<unsigned>(Number(fields[3]));
				report.AssertionsPassed = static_cast<unsigned>(Number(fields[4]));
				report.AssertionsFailed = static_cast<unsigned>(Number(fields[5]));
				totalled = true;
			}
		}

		return totalled;
	}

	bool ReadSuiteReport(const fs::path &path, SuiteReport &report) {
		std::ifstream file(path);
		if (!file) {
			return false;
		}

		std::ostringstream contents;
		contents << file.rdbuf();
		return ParseSuiteReport(contents.str(), report);
	}

	// -----------------------------------------------------------------------
	// The documents
	// -----------------------------------------------------------------------

	std::string RenderMarkdown(const std::vector<SuiteReport> &suites) {
		const auto ordered = Ordered(suites);
		const Tally overall = TallyOf(ordered);

		std::ostringstream page;
		page << "# Test results\n\n";
		page << (overall.Green() ? "**All suites green.** " : "**Failing.** ");
		page << Sentence(overall) << "\n\n";
		page << overall.SuitesGreen << " of " << overall.Suites << " suite(s) green. " << overall.SuitesRan
			 << " ran; " << (overall.Suites - overall.SuitesRan)
			 << " were unchanged and carried forward from the cascade cache.\n";

		// No timestamp. Two runs that learned the same thing produce the same
		// bytes, so a diff of this file shows what moved rather than that it was
		// written again.
		for (const auto &top : GroupBy(ordered, 1)) {
			const Tally section = TallyOf(top.Suites);
			page << "\n## " << top.Name << "\n\n" << Sentence(section) << "\n";

			for (const auto &middle : GroupBy(top.Suites, 2)) {
				page << "\n### " << middle.Name << "\n\n";
				page << "| Suite | Cases | Passed | Failed | Skipped | Assertions | Time | Slowest case |"
						" Result |\n";
				page << "|---|--:|--:|--:|--:|--:|--:|--:|---|\n";

				for (const auto *suite : middle.Suites) {
					page << "| `" << suite->Id << "` | " << suite->CaseCount() << " | " << suite->CasesPassed
						 << " | " << suite->CasesFailed << " | " << suite->CasesSkipped << " | "
						 << (suite->AssertionsPassed + suite->AssertionsFailed) << " | "
						 << FormatDuration(suite->Microseconds) << " | " << Slowest(*suite) << " | "
						 << Outcome(*suite) << " |\n";
				}

				for (const auto *suite : middle.Suites) {
					std::vector<const CaseReport *> failures;
					for (const auto &entry : suite->Cases) {
						if (entry.Failed) {
							failures.push_back(&entry);
						}
					}
					if (failures.empty()) {
						continue;
					}

					page << "\nFailing cases in `" << suite->Id << "`:\n\n";
					for (const auto *failure : failures) {
						page << "- " << failure->Name << " (" << FormatDuration(failure->Microseconds)
							 << ")\n";
					}
				}
			}
		}

		return page.str();
	}

	std::string RenderHtml(const std::vector<SuiteReport> &suites) {
		const auto ordered = Ordered(suites);
		const Tally overall = TallyOf(ordered);

		std::ostringstream page;
		page << "<!doctype html>\n<html lang=\"en\">\n<head>\n";
		page << "<meta charset=\"utf-8\">\n";
		page << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
		page << "<title>Test results</title>\n";

		// One file, no requests. A report that needs a network to render is a
		// report that does not render on the machine that produced it.
		page
			<< "<style>\n"
			   ":root { color-scheme: light dark;\n"
			   "  --ink: #14171a; --dim: #5c6670; --paper: #ffffff; --panel: #f4f6f8;\n"
			   "  --line: #d8dee4; --pass: #1a7f37; --fail: #b42318; --skip: #9a6700; }\n"
			   "@media (prefers-color-scheme: dark) { :root {\n"
			   "  --ink: #e6edf3; --dim: #8b949e; --paper: #0d1117; --panel: #161b22;\n"
			   "  --line: #30363d; --pass: #3fb950; --fail: #f85149; --skip: #d29922; } }\n"
			   "* { box-sizing: border-box; }\n"
			   "body { margin: 0; padding: 2.5rem 1.5rem 5rem; background: var(--paper); color: var(--ink);\n"
			   "  font: 15px/1.55 ui-sans-serif, system-ui, -apple-system, Segoe UI, Roboto, sans-serif; }\n"
			   "main { max-width: 62rem; margin: 0 auto; }\n"
			   "h1 { font-size: 1.6rem; margin: 0 0 .35rem; }\n"
			   "h2 { font-size: 1.2rem; margin: 2.5rem 0 .25rem; padding-bottom: .35rem;\n"
			   "  border-bottom: 1px solid var(--line); }\n"
			   "h3 { font-size: .95rem; margin: 1.75rem 0 .5rem; color: var(--dim);\n"
			   "  font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-weight: 600; }\n"
			   "p { margin: .25rem 0; color: var(--dim); }\n"
			   ".verdict { font-weight: 600; color: var(--pass); }\n"
			   ".verdict.bad { color: var(--fail); }\n"
			   ".bar { height: .4rem; border-radius: .2rem; background: var(--panel);\n"
			   "  overflow: hidden; margin: .75rem 0 .25rem; }\n"
			   ".bar > span { display: block; height: 100%; background: var(--pass); }\n"
			   ".bar.bad > span { background: var(--fail); }\n"
			   "table { width: 100%; border-collapse: collapse; margin: .5rem 0 0; font-size: .9rem; }\n"
			   "caption { text-align: left; }\n"
			   "th, td { padding: .4rem .6rem; border-bottom: 1px solid var(--line); text-align: right; }\n"
			   "th:first-child, td:first-child, th:last-child, td:last-child { text-align: left; }\n"
			   "th { color: var(--dim); font-weight: 600; font-size: .78rem;\n"
			   "  text-transform: uppercase; letter-spacing: .04em; }\n"
			   "td.suite { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }\n"
			   "td.pass { color: var(--pass); } td.fail { color: var(--fail); font-weight: 600; }\n"
			   "td.cached { color: var(--dim); }\n"
			   ".failures { background: var(--panel); border-left: 3px solid var(--fail);\n"
			   "  padding: .6rem .9rem; margin: .75rem 0 0; border-radius: 0 .25rem .25rem 0; }\n"
			   ".failures ul { margin: .3rem 0 0; padding-left: 1.1rem; }\n"
			   ".failures li { color: var(--ink); }\n"
			   ".wrap { overflow-x: auto; }\n"
			   // The flamegraph. Nested divs whose widths are percentages of
			   // their parent, which is the one layout that needs no script to
			   // stay proportional: the browser does the arithmetic on resize.
			   ".flame { margin: 1rem 0 0; }\n"
			   ".flame .row { display: flex; align-items: flex-start; }\n"
			   ".flame .frame { min-width: 0; }\n"
			   ".flame .box { height: 1.15rem; margin: 1px; border-radius: 2px; overflow: hidden;\n"
			   "  white-space: nowrap; font-size: .68rem; line-height: 1.15rem; color: #1b1206;\n"
			   "  padding: 0 .3rem; cursor: default; }\n"
			   ".flame .box:hover { outline: 2px solid var(--ink); outline-offset: -2px; }\n"
			   ".flame .box > span { pointer-events: none; }\n"
			   // The hover readout. Fixed to the viewport rather than to the
			   // box, which is what makes it work at every width: a box one
			   // pixel wide at the right-hand edge would otherwise carry a
			   // panel hanging off the page, and CSS cannot measure the edge to
			   // avoid it. Pinned to the bottom it is the same size in the same
			   // place every time, so reading one after another is a comparison
			   // rather than a hunt.
			   ".flame .tip { display: none; position: fixed; left: 1rem; right: 1rem; bottom: 1rem;\n"
			   "  z-index: 20; pointer-events: none; background: var(--panel); color: var(--ink);\n"
			   "  border: 1px solid var(--line); border-radius: .4rem; padding: .55rem .8rem;\n"
			   "  box-shadow: 0 6px 24px rgba(0,0,0,.28); font-size: .82rem; }\n"
			   // `~` rather than a descendant selector: a child frame's box
			   // lives in .row, a sibling of this box, so hovering a case shows
			   // the case's readout and never also its suite's.
			   ".flame .box:hover ~ .tip { display: block; }\n"
			   ".flame .tip b { display: block; font-size: .88rem; margin-bottom: .1rem;\n"
			   "  font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }\n"
			   ".flame .tip span { color: var(--dim); }\n"
			   ".legend { display: flex; gap: 1.1rem; flex-wrap: wrap; margin: .6rem 0 0;\n"
			   "  font-size: .78rem; color: var(--dim); }\n"
			   ".legend i { display: inline-block; width: .7rem; height: .7rem; border-radius: 2px;\n"
			   "  margin-right: .35rem; vertical-align: -1px; }\n"
			   "</style>\n</head>\n<body>\n<main>\n";

		page << "<h1>Test results</h1>\n";
		page << "<p class=\"verdict" << (overall.Green() ? "" : " bad") << "\">"
			 << (overall.Green() ? "All suites green." : "Failing.") << "</p>\n";
		page << "<div class=\"bar" << (overall.Green() ? "" : " bad")
			 << "\"><span style=\"width:" << overall.Percent() << "%\"></span></div>\n";
		page << "<p>" << Sentence(overall) << "</p>\n";
		page << "<p>" << overall.SuitesGreen << " of " << overall.Suites << " suite(s) green &middot; "
			 << overall.SuitesRan << " ran &middot; " << (overall.Suites - overall.SuitesRan)
			 << " carried forward from the cascade cache.</p>\n";

		if (overall.Microseconds > 0) {
			page << "<h2>Where the time went</h2>\n";
			page << "<p>Width is wall-clock. Sections are the identifier, one component at a time; the "
					"bottom row is individual cases. Hover a box and the full reading appears along the "
					"bottom of the window.</p>\n";
			page << "<div class=\"flame\">";
			RenderFrame(page, BuildFlamegraph(ordered), 100.0, {});
			page << "</div>\n";
			page << "<div class=\"legend\">"
					"<span><i style=\"background:hsl(30 72% 58%)\"></i>ran this time</span>"
					"<span><i style=\"background:hsl(30 22% 58%)\"></i>carried forward from the cache, so "
					"no per-case breakdown</span>"
					"<span><i style=\"background:hsl(2 72% 52%)\"></i>failing</span>"
					"</div>\n";
			page << "<p>A suite is wider than its cases add up to. The remainder is starting the process "
					"and running its static initialisers, which every suite pays once.</p>\n";
		}

		for (const auto &top : GroupBy(ordered, 1)) {
			const Tally section = TallyOf(top.Suites);
			page << "<h2>" << Escape(top.Name) << "</h2>\n";
			page << "<div class=\"bar" << (section.Green() ? "" : " bad")
				 << "\"><span style=\"width:" << section.Percent() << "%\"></span></div>\n";
			page << "<p>" << Sentence(section) << "</p>\n";

			for (const auto &middle : GroupBy(top.Suites, 2)) {
				page << "<h3>" << Escape(middle.Name) << "</h3>\n";
				page << "<div class=\"wrap\"><table>\n<thead><tr><th>Suite</th><th>Cases</th>"
						"<th>Passed</th><th>Failed</th><th>Skipped</th><th>Assertions</th>"
						"<th>Time</th><th>Slowest case</th><th>Result</th></tr></thead>\n<tbody>\n";

				for (const auto *suite : middle.Suites) {
					const char *state = !suite->Passed ? "fail" : (suite->Ran ? "pass" : "cached");
					page << "<tr><td class=\"suite\">" << Escape(suite->Id) << "</td><td>"
						 << suite->CaseCount() << "</td><td>" << suite->CasesPassed << "</td><td>"
						 << suite->CasesFailed << "</td><td>" << suite->CasesSkipped << "</td><td>"
						 << (suite->AssertionsPassed + suite->AssertionsFailed) << "</td><td>"
						 << FormatDuration(suite->Microseconds) << "</td><td>" << Escape(Slowest(*suite))
						 << "</td><td class=\"" << state << "\">" << Outcome(*suite) << "</td></tr>\n";
				}

				page << "</tbody>\n</table></div>\n";

				for (const auto *suite : middle.Suites) {
					std::vector<const CaseReport *> failures;
					for (const auto &entry : suite->Cases) {
						if (entry.Failed) {
							failures.push_back(&entry);
						}
					}
					if (failures.empty()) {
						continue;
					}

					page << "<div class=\"failures\">Failing cases in <code>" << Escape(suite->Id)
						 << "</code>:\n<ul>\n";
					for (const auto *failure : failures) {
						page << "<li>" << Escape(failure->Name) << " <em>("
							 << Escape(FormatDuration(failure->Microseconds)) << ")</em></li>\n";
					}
					page << "</ul>\n</div>\n";
				}
			}
		}

		page << "</main>\n</body>\n</html>\n";
		return page.str();
	}

	bool WriteReports(const fs::path &directory, const std::vector<SuiteReport> &suites) {
		std::error_code error;
		fs::create_directories(directory, error);

		bool written = true;

		std::ofstream markdown(directory / "test-output.md", std::ios::trunc);
		markdown << RenderMarkdown(suites);
		written = written && markdown.good();

		std::ofstream html(directory / "test-output.html", std::ios::trunc);
		html << RenderHtml(suites);
		written = written && html.good();

		return written;
	}
}
