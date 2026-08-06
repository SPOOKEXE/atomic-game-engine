#pragma once

// The half that owns a terminal: raw mode, the alternate screen, a key, and
// the escape sequences a frame is made of.
//
// **Everything that can be decided without a terminal is decided in
// `Dashboard.hpp`.** What is left here is genuinely a file descriptor's — and
// even of that, only `Terminal` itself is: key decoding, scroll arithmetic and
// frame composition are free functions over values, so a suite exercises the
// whole of what an operator sees and presses without opening a tty. The one
// piece with no unit suite is `Terminal::Open`, for `Renderer.hpp`'s reason —
// it needs the device, and a mock of one would close the gap on paper only.
//
// **Raw mode swallows Ctrl-C on purpose.** Leaving `ISIG` on would let the
// signal end the process with the terminal still in raw mode, no cursor and the
// alternate screen up — a shell nobody can type into, from a program that had a
// perfectly good destructor. Ctrl-C is decoded as `Key::Quit` instead, and the
// loop leaves the way `q` does.
//
// **Nothing here blocks.** `Read` returns whatever has arrived and no more,
// because the thread it is called on is also the thread serving content, and a
// keypress is not worth a stalled origin.
//
// @tier shared

#include <cdn/Dashboard.hpp>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace cdn {

	// A key the dashboard acts on.
	//
	// A closed list rather than a character, because the same action arrives
	// spelled three ways — an arrow key is an escape sequence, `j` is a byte,
	// and both mean "down".
	//
	// @since v0.9
	enum class Key : uint8_t {
		// Nothing decodable yet.
		None,

		// Up one line.
		Up,

		// Down one line.
		Down,

		// Up one screen.
		PageUp,

		// Down one screen.
		PageDown,

		// The top of the document.
		Home,

		// The bottom.
		End,

		// Leave.
		Quit,
	};

	// One decoded keypress and what it cost.
	//
	// @since v0.9
	struct KeyPress {
		// What was pressed.
		Key Pressed = Key::None;

		// How many bytes it consumed. **Zero means "not yet"** — the buffer
		// holds the start of an escape sequence whose remaining bytes have not
		// arrived, and a caller that dropped them would turn one arrow key into
		// a stray `[` in the input. Keep the remainder and decode again after
		// the next read.
		size_t Consumed = 0;
	};

	// Decodes one keypress from the front of a run of input bytes.
	//
	// @param bytes What has arrived and not been consumed.
	// @return The key and how much of the buffer it took.
	// @since v0.9
	KeyPress DecodeKey(std::string_view bytes);

	// How big the screen is, in character cells.
	//
	// @since v0.9
	struct ScreenSize {
		// Columns across.
		size_t Columns = 80;

		// Rows down, the status bar included.
		size_t Rows = 24;
	};

	// Where the scrolled view is.
	//
	// @since v0.9
	struct Viewport {
		// The document line drawn at the top of the screen.
		size_t Top = 0;

		// Moves the view.
		//
		// **The bottom of the document is the last screenful, not the last
		// line.** Scrolling until one row is left above an empty screen is the
		// behaviour of a text box rather than of a pager, and it makes the end
		// of a long asset list hard to read.
		//
		// @param key What was pressed.
		// @param lines How many lines the document has.
		// @param visibleRows How many of them fit, the status bar excluded.
		void Apply(Key key, size_t lines, size_t visibleRows);
	};

	// Cuts text to a column count without splitting a character.
	//
	// The sparkline is block glyphs and an asset's name is whatever a game
	// author typed, so both are UTF-8 and both reach the right-hand edge of a
	// narrow terminal. Cutting on a byte would leave half a codepoint on the
	// line, which most terminals draw as a replacement box and some draw by
	// eating the next character.
	//
	// **A column is counted as a codepoint**, which is right for the content
	// here and wrong in general — a combining mark or a wide CJK glyph is not
	// one column. Say so rather than implying this is a width calculation.
	//
	// @param text The line.
	// @param columns The most to keep.
	// @return The text, cut on a character boundary.
	// @since v0.9
	std::string TrimToColumns(std::string_view text, size_t columns);

	// Builds one whole frame, escape sequences and all.
	//
	// **One string, written in one call.** Drawing a screen with a write per
	// line lets a terminal display half of it, which reads as flicker on every
	// redraw — and at four redraws a second it is the difference between a
	// dashboard somebody leaves open and one they close.
	//
	// @param dashboard What to draw.
	// @param view Where the document is scrolled to.
	// @param size How big the screen is.
	// @return The frame.
	// @since v0.9
	std::string RenderFrame(const Dashboard &dashboard, const Viewport &view, const ScreenSize &size);

	// A terminal in raw mode, on the alternate screen.
	//
	// **One owner, one thread**, the same as everything else here that owns a
	// descriptor. The destructor restores whatever `Open` changed, which is the
	// only reason it is safe for this to change anything at all.
	//
	// @since v0.9
	class Terminal {
	  public:
		// Restores the terminal.
		virtual ~Terminal() = default;

		// How big the screen is now.
		//
		// Asked every frame rather than cached: a window is resized while a
		// program is running, and a dashboard that kept the size it started
		// with draws into a screen that is no longer there.
		virtual ScreenSize Size() const = 0;

		// Takes whatever has been typed and not yet read.
		//
		// @return The bytes, empty when nothing is waiting.
		virtual std::string Read() = 0;

		// Writes one frame.
		//
		// @param frame What `RenderFrame` produced.
		virtual void Present(std::string_view frame) = 0;

		// Restores the terminal now rather than at destruction.
		//
		// Idempotent, because the destructor calls it too.
		virtual void Close() = 0;

		// Takes over this process's terminal.
		//
		// @return The terminal, or nothing when there is not one — output
		//         redirected to a file or a pipe, a service manager's log, a
		//         CI job. **A refusal rather than a guess**: escape sequences
		//         written into a log file are a log nobody can read, and the
		//         caller can carry on without a dashboard.
		static std::unique_ptr<Terminal> Open();
	};
}
