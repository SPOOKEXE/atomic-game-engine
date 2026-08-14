#include <array>
#include <cdn/Terminal.hpp>
#include <windows.h>

// The device half, on Windows.
//
// **The escape sequences are the same ones the POSIX half writes, and that is
// the point.** `ENABLE_VIRTUAL_TERMINAL_PROCESSING` makes the console read them
// and `ENABLE_VIRTUAL_TERMINAL_INPUT` makes it *write* them for arrow keys, so
// `DecodeKey` and `RenderFrame` are one implementation rather than two. A
// console-API path would have needed its own key mapping, which is a second
// place for `PgUp` to mean something slightly different.
//
// Both flags are Windows 10 1511 and later. An older console fails the mode
// change and this refuses to open, which the caller reports and carries on
// without - the same outcome as a redirected stdout.

namespace cdn {
	namespace {
		constexpr std::string_view ENTERING = "\x1b[?1049h\x1b[?25l\x1b[2J";
		constexpr std::string_view LEAVING = "\x1b[?25h\x1b[?1049l";

		void WriteAll(HANDLE output, std::string_view bytes) {
			size_t written = 0;
			while (written < bytes.size()) {
				DWORD went = 0;
				if (!::WriteFile(
						output,
						bytes.data() + written,
						static_cast<DWORD>(bytes.size() - written),
						&went,
						nullptr
					) ||
					went == 0) {
					return;
				}
				written += went;
			}
		}

		class WindowsTerminal final : public Terminal {
		  public:
			WindowsTerminal(HANDLE input, HANDLE output, DWORD inputMode, DWORD outputMode)
				: Input(input), Output(output), RestoredInput(inputMode), RestoredOutput(outputMode) {}

			~WindowsTerminal() override {
				Close();
			}

			ScreenSize Size() const override {
				CONSOLE_SCREEN_BUFFER_INFO info{};
				if (!::GetConsoleScreenBufferInfo(Output, &info)) {
					return {};
				}
				const int columns = info.srWindow.Right - info.srWindow.Left + 1;
				const int rows = info.srWindow.Bottom - info.srWindow.Top + 1;
				if (columns <= 0 || rows <= 0) {
					return {};
				}
				return {static_cast<size_t>(columns), static_cast<size_t>(rows)};
			}

			std::string Read() override {
				// Read only what the console says is already there. A blocking
				// read here would stop the origin serving until somebody typed.
				std::string typed;
				std::array<char, 64> buffer{};
				for (;;) {
					DWORD pending = 0;
					if (!::GetNumberOfConsoleInputEvents(Input, &pending) || pending == 0) {
						break;
					}
					DWORD taken = 0;
					if (!::ReadFile(
							Input, buffer.data(), static_cast<DWORD>(buffer.size()), &taken, nullptr
						) ||
						taken == 0) {
						break;
					}
					typed.append(buffer.data(), taken);
				}
				return typed;
			}

			void Present(std::string_view frame) override {
				WriteAll(Output, frame);
			}

			void Close() override {
				if (Closed) {
					return;
				}
				Closed = true;
				WriteAll(Output, LEAVING);
				::SetConsoleMode(Input, RestoredInput);
				::SetConsoleMode(Output, RestoredOutput);
			}

		  private:
			HANDLE Input;
			HANDLE Output;
			DWORD RestoredInput;
			DWORD RestoredOutput;
			bool Closed = false;
		};
	}

	std::unique_ptr<Terminal> Terminal::Open() {
		HANDLE input = ::GetStdHandle(STD_INPUT_HANDLE);
		HANDLE output = ::GetStdHandle(STD_OUTPUT_HANDLE);
		if (input == INVALID_HANDLE_VALUE || output == INVALID_HANDLE_VALUE) {
			return nullptr;
		}

		DWORD inputMode = 0;
		DWORD outputMode = 0;
		// A redirected handle is not a console and fails here, which is the
		// check `isatty` is on the other platform.
		if (!::GetConsoleMode(input, &inputMode) || !::GetConsoleMode(output, &outputMode)) {
			return nullptr;
		}

		// `ENABLE_PROCESSED_INPUT` off is the counterpart of `ISIG` off: Ctrl-C
		// arrives as a byte and is decoded as `Key::Quit`, so leaving runs the
		// destructor that puts the console back.
		const DWORD raw =
			(inputMode &
			 ~static_cast<DWORD>(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT)) |
			ENABLE_VIRTUAL_TERMINAL_INPUT;
		if (!::SetConsoleMode(input, raw)) {
			return nullptr;
		}
		if (!::SetConsoleMode(output, outputMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
			::SetConsoleMode(input, inputMode);
			return nullptr;
		}

		WriteAll(output, ENTERING);
		return std::make_unique<WindowsTerminal>(input, output, inputMode, outputMode);
	}
}
