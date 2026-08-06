#include <algorithm>
#include <array>
#include <cdn/Terminal.hpp>
#include <cstdio>

// The parts of a terminal that are not a device: what a key means, where the
// view is, and what a frame is made of. `platform/` holds the rest, which is
// the descriptor and the two calls that change its mode.

namespace cdn {
	namespace {
		// What an escape sequence means. Both spellings of Home and End are
		// here because a terminal picks one and there is no way to ask which:
		// xterm sends `\x1b[H`, and a terminal in application-keypad mode sends
		// `\x1b[1~`.
		struct Sequence {
			std::string_view Bytes;
			Key Means;
		};

		constexpr std::array<Sequence, 8> SEQUENCES = {
			Sequence{"\x1b[A", Key::Up},
			Sequence{"\x1b[B", Key::Down},
			Sequence{"\x1b[5~", Key::PageUp},
			Sequence{"\x1b[6~", Key::PageDown},
			Sequence{"\x1b[H", Key::Home},
			Sequence{"\x1b[F", Key::End},
			Sequence{"\x1b[1~", Key::Home},
			Sequence{"\x1b[4~", Key::End},
		};

		// Ctrl-C, which raw mode delivers as a byte rather than as a signal.
		constexpr char INTERRUPT = '\x03';

		// The colour a style is drawn in. Bold and one colour rather than a
		// palette: a terminal's palette is the operator's, and a dashboard that
		// insisted on its own would be unreadable on half of them.
		std::string_view Escape(LineStyle style) {
			switch (style) {
			case LineStyle::Heading:
				return "\x1b[1;36m";
			case LineStyle::Emphasis:
				return "\x1b[1m";
			case LineStyle::Row:
			case LineStyle::Blank:
				break;
			}
			return "";
		}

		// Whether a byte starts a UTF-8 character rather than continuing one.
		bool StartsCharacter(char byte) {
			return (static_cast<unsigned char>(byte) & 0xC0) != 0x80;
		}

		// How many cells text occupies, on `TrimToColumns`' terms.
		size_t Columns(std::string_view text) {
			size_t characters = 0;
			for (const char byte : text) {
				characters += StartsCharacter(byte) ? 1 : 0;
			}
			return characters;
		}
	}

	KeyPress DecodeKey(std::string_view bytes) {
		if (bytes.empty()) {
			return {};
		}

		if (bytes.front() == '\x1b') {
			for (const Sequence &sequence : SEQUENCES) {
				if (bytes.starts_with(sequence.Bytes)) {
					return {sequence.Means, sequence.Bytes.size()};
				}
			}
			for (const Sequence &sequence : SEQUENCES) {
				if (sequence.Bytes.starts_with(bytes)) {
					// The start of a sequence whose tail has not arrived. Asking
					// for it again is right; consuming it would turn one arrow
					// key into a stray bracket in the input.
					return {};
				}
			}
			// Escape pressed on its own, or a sequence this does not know. One
			// byte, so whatever follows still decodes.
			return {Key::None, 1};
		}

		switch (bytes.front()) {
		case 'q':
		case 'Q':
		case INTERRUPT:
			return {Key::Quit, 1};
		case 'k':
			return {Key::Up, 1};
		case 'j':
			return {Key::Down, 1};
		case ' ':
		case 'f':
			return {Key::PageDown, 1};
		case 'b':
			return {Key::PageUp, 1};
		case 'g':
			return {Key::Home, 1};
		case 'G':
			return {Key::End, 1};
		default:
			break;
		}
		return {Key::None, 1};
	}

	void Viewport::Apply(Key key, size_t lines, size_t visibleRows) {
		const size_t furthest = lines > visibleRows ? lines - visibleRows : 0;
		// A page is a screen less one line, so the row an operator was reading
		// when they pressed the key is still on the screen afterwards.
		const size_t page = visibleRows > 1 ? visibleRows - 1 : 1;

		switch (key) {
		case Key::Up:
			Top = Top > 0 ? Top - 1 : 0;
			break;
		case Key::Down:
			Top += 1;
			break;
		case Key::PageUp:
			Top = Top > page ? Top - page : 0;
			break;
		case Key::PageDown:
			Top += page;
			break;
		case Key::Home:
			Top = 0;
			break;
		case Key::End:
			Top = furthest;
			break;
		case Key::None:
		case Key::Quit:
			break;
		}
		Top = std::min(Top, furthest);
	}

	std::string TrimToColumns(std::string_view text, size_t columns) {
		size_t kept = 0;
		size_t characters = 0;
		while (kept < text.size()) {
			if (StartsCharacter(text[kept])) {
				if (characters == columns) {
					break;
				}
				++characters;
			}
			++kept;
		}
		return std::string(text.substr(0, kept));
	}

	std::string RenderFrame(const Dashboard &dashboard, const Viewport &view, const ScreenSize &size) {
		const size_t visibleRows = size.Rows > 1 ? size.Rows - 1 : 1;
		const size_t lines = dashboard.Lines();

		std::string frame;
		// Home the cursor rather than clearing the screen: a clear draws one
		// blank frame between every two real ones, which is what flicker is.
		frame += "\x1b[H";

		for (size_t row = 0; row < visibleRows; ++row) {
			const DashboardLine line = dashboard.LineAt(view.Top + row);
			const std::string_view colour = Escape(line.Style);

			frame += "\x1b[2K"; // Clear the line, so a shorter one does not
								// leave the tail of the last frame behind.
			frame += colour;
			frame += TrimToColumns(line.Text, size.Columns);
			if (!colour.empty()) {
				frame += "\x1b[0m";
			}
			frame += "\r\n";
		}

		const size_t first = lines == 0 ? 0 : std::min(view.Top + 1, lines);
		const size_t last = std::min(view.Top + visibleRows, lines);

		std::array<char, 64> position{};
		std::snprintf(position.data(), position.size(), "%zu-%zu of %zu ", first, last, lines);
		const std::string_view where(position.data());

		std::string status =
			TrimToColumns(" q quit · ↑↓ jk scroll · PgUp/PgDn b/space page · g/G ends", size.Columns);

		// The position is dropped rather than wrapped on a terminal too narrow
		// for both: a status bar that wrapped would push the document's last
		// line off the screen and scroll everything by one.
		const size_t used = Columns(status);
		if (used + where.size() <= size.Columns) {
			status += std::string(size.Columns - used - where.size(), ' ');
			status += where;
		}

		frame += "\x1b[2K\x1b[7m";
		frame += status;
		frame += "\x1b[0m";
		return frame;
	}
}
