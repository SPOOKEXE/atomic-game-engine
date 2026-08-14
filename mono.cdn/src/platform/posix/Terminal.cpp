#include <array>
#include <cdn/Terminal.hpp>
#include <cerrno>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

// The device half, on everything that is not Windows.
//
// **Every mode this changes is changed back in one place.** A terminal left in
// raw mode with the cursor hidden is a shell the operator has to reset by hand,
// and the ways out of this program are more numerous than they look: `q`, a
// closed socket, an exception on the way up, and Ctrl-C - which raw mode
// delivers as a byte precisely so that the destructor still runs.

namespace cdn {
	namespace {
		// Enter the alternate screen, hide the cursor, clear. The alternate
		// screen is what gives the operator their scrollback back when the
		// program exits, rather than an hour of redrawn frames in it.
		constexpr std::string_view ENTERING = "\x1b[?1049h\x1b[?25l\x1b[2J";

		// Show the cursor, leave the alternate screen. The reverse order, so
		// the cursor is restored on the screen it belongs to.
		constexpr std::string_view LEAVING = "\x1b[?25h\x1b[?1049l";

		// Writes the whole of a buffer, or as much as the descriptor will take.
		//
		// A short write on a terminal is ordinary and a partial frame is not
		// worth having, so this loops. `EINTR` is retried; anything else means
		// the terminal has gone, and the next `Size` call is where that is
		// noticed.
		void WriteAll(int descriptor, std::string_view bytes) {
			size_t written = 0;
			while (written < bytes.size()) {
				const ssize_t went = ::write(descriptor, bytes.data() + written, bytes.size() - written);
				if (went > 0) {
					written += static_cast<size_t>(went);
					continue;
				}
				if (went < 0 && errno == EINTR) {
					continue;
				}
				return;
			}
		}

		class PosixTerminal final : public Terminal {
		  public:
			explicit PosixTerminal(const termios &original) : Restored(original) {}

			~PosixTerminal() override {
				Close();
			}

			ScreenSize Size() const override {
				winsize window{};
				if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0 || window.ws_col == 0 ||
					window.ws_row == 0) {
					// A terminal that will not say how big it is gets the
					// default rather than a zero-row screen, which would draw
					// nothing and look like a hung program.
					return {};
				}
				return {static_cast<size_t>(window.ws_col), static_cast<size_t>(window.ws_row)};
			}

			std::string Read() override {
				// `VMIN` and `VTIME` are both zero, so a read with nothing
				// waiting returns zero rather than blocking. That is what keeps
				// the serving loop a serving loop.
				std::string typed;
				std::array<char, 64> buffer{};
				for (;;) {
					const ssize_t taken = ::read(STDIN_FILENO, buffer.data(), buffer.size());
					if (taken <= 0) {
						break;
					}
					typed.append(buffer.data(), static_cast<size_t>(taken));
					if (static_cast<size_t>(taken) < buffer.size()) {
						break;
					}
				}
				return typed;
			}

			void Present(std::string_view frame) override {
				WriteAll(STDOUT_FILENO, frame);
			}

			void Close() override {
				if (Closed) {
					return;
				}
				Closed = true;
				WriteAll(STDOUT_FILENO, LEAVING);
				::tcsetattr(STDIN_FILENO, TCSAFLUSH, &Restored);
			}

		  private:
			termios Restored;
			bool Closed = false;
		};
	}

	std::unique_ptr<Terminal> Terminal::Open() {
		if (::isatty(STDIN_FILENO) != 1 || ::isatty(STDOUT_FILENO) != 1) {
			return nullptr;
		}

		termios original{};
		if (::tcgetattr(STDIN_FILENO, &original) != 0) {
			return nullptr;
		}

		termios raw = original;
		// `ISIG` off is the deliberate one: Ctrl-C arrives as a byte and is
		// decoded as `Key::Quit`, so the way out runs the destructor that puts
		// all of this back. `OPOST` off means a bare newline no longer becomes
		// a carriage return too, which is why every frame writes `\r\n` itself.
		raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
		raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
		raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		if (::tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
			return nullptr;
		}

		WriteAll(STDOUT_FILENO, ENTERING);
		return std::make_unique<PosixTerminal>(original);
	}
}
