#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cdn/Terminal.hpp>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

TEST_SUITE_ID("cdn.terminal")
TEST_DEPENDS("cdn.dashboard")

using cdn::ContentRoot;
using cdn::Dashboard;
using cdn::DecodeKey;
using cdn::Key;
using cdn::KeyPress;
using cdn::Publication;
using cdn::RenderFrame;
using cdn::ScreenSize;
using cdn::StoreFootprint;
using cdn::TrimToColumns;
using cdn::Viewport;
using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::Manifest;

namespace {
	namespace fs = std::filesystem;

	struct Tree {
		fs::path Root;

		Tree() {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-terminal-" + std::to_string(++serial));
			std::error_code failure;
			fs::remove_all(Root, failure);
			fs::create_directories(Root);
		}

		~Tree() {
			std::error_code failure;
			fs::remove_all(Root, failure);
		}

		ContentRoot Mount() const {
			auto root = ContentRoot::Mount(Root);
			REQUIRE(root.has_value());
			return *root;
		}
	};

	ChunkEntry Sized(std::string_view name, uint32_t bytes) {
		ChunkEntry entry;
		entry.Hash = Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(name.data()), name.size())
		);
		entry.Bytes = bytes;
		return entry;
	}

	// Enough assets that the document is longer than any screen a case here
	// asks for, so scrolling has somewhere to go.
	Publication Content(const Tree &tree, size_t assets = 40) {
		Manifest manifest;
		std::vector<ContentHash> roots;
		for (size_t index = 0; index < assets; ++index) {
			const std::string name = "meshes/asset-" + std::to_string(index) + ".mesh";
			roots.push_back(
				manifest.AddAsset(name, AssetKind::Mesh, {Sized(name, static_cast<uint32_t>(1024 + index))})
			);
		}
		REQUIRE(manifest.AddBundle(roots).has_value());
		return Publication(tree.Mount(), std::move(manifest));
	}

	size_t CountLines(std::string_view frame) {
		size_t lines = 0;
		for (size_t at = frame.find("\r\n"); at != std::string_view::npos; at = frame.find("\r\n", at + 2)) {
			++lines;
		}
		return lines;
	}
}

TEST_CASE("a typed byte decodes to what it means", "[cdn][terminal]") {
	CHECK(DecodeKey("q").Pressed == Key::Quit);
	CHECK(DecodeKey("Q").Pressed == Key::Quit);
	// Raw mode delivers Ctrl-C as a byte rather than as a signal, precisely so
	// that leaving runs the destructor that puts the terminal back.
	CHECK(DecodeKey("\x03").Pressed == Key::Quit);
	CHECK(DecodeKey("j").Pressed == Key::Down);
	CHECK(DecodeKey("k").Pressed == Key::Up);
	CHECK(DecodeKey(" ").Pressed == Key::PageDown);
	CHECK(DecodeKey("b").Pressed == Key::PageUp);
	CHECK(DecodeKey("g").Pressed == Key::Home);
	CHECK(DecodeKey("G").Pressed == Key::End);

	// Anything else is consumed rather than left to jam the buffer.
	const KeyPress unknown = DecodeKey("z");
	CHECK(unknown.Pressed == Key::None);
	CHECK(unknown.Consumed == 1);
}

TEST_CASE("an escape sequence decodes whole", "[cdn][terminal]") {
	CHECK(DecodeKey("\x1b[A").Pressed == Key::Up);
	CHECK(DecodeKey("\x1b[B").Pressed == Key::Down);
	CHECK(DecodeKey("\x1b[5~").Pressed == Key::PageUp);
	CHECK(DecodeKey("\x1b[6~").Pressed == Key::PageDown);
	// Both spellings, because a terminal picks one and there is no way to ask
	// which it picked.
	CHECK(DecodeKey("\x1b[H").Pressed == Key::Home);
	CHECK(DecodeKey("\x1b[1~").Pressed == Key::Home);
	CHECK(DecodeKey("\x1b[F").Pressed == Key::End);
	CHECK(DecodeKey("\x1b[4~").Pressed == Key::End);

	CHECK(DecodeKey("\x1b[A").Consumed == 3);
	CHECK(DecodeKey("\x1b[6~").Consumed == 4);
}

TEST_CASE("a half-arrived escape sequence is waited for, not eaten", "[cdn][terminal]") {
	// Three bytes do not always arrive in one read. Consuming the prefix would
	// turn one arrow key into a stray bracket in the input, and the caller
	// keeps the remainder for exactly this reason.
	CHECK(DecodeKey("\x1b").Consumed == 0);
	CHECK(DecodeKey("\x1b[").Consumed == 0);
	CHECK(DecodeKey("\x1b[6").Consumed == 0);

	// An escape followed by something that is not a sequence this knows is one
	// byte, so whatever came after it still decodes.
	const KeyPress stray = DecodeKey(
		"\x1b"
		"q"
	);
	CHECK(stray.Consumed == 1);
	CHECK(stray.Pressed == Key::None);
	CHECK(DecodeKey("q").Pressed == Key::Quit);
}

TEST_CASE("decoding a run of bytes takes them in order", "[cdn][terminal]") {
	std::string typed = "\x1b[Bj\x1b[5~q";
	std::vector<Key> pressed;
	for (;;) {
		const KeyPress press = DecodeKey(typed);
		if (press.Consumed == 0) {
			break;
		}
		typed.erase(0, press.Consumed);
		pressed.push_back(press.Pressed);
	}

	REQUIRE(pressed.size() == 4);
	CHECK(pressed[0] == Key::Down);
	CHECK(pressed[1] == Key::Down);
	CHECK(pressed[2] == Key::PageUp);
	CHECK(pressed[3] == Key::Quit);
	CHECK(typed.empty());
}

TEST_CASE("the view cannot scroll past either end", "[cdn][terminal]") {
	Viewport view;
	view.Apply(Key::Up, 100, 10);
	CHECK(view.Top == 0);

	view.Apply(Key::End, 100, 10);
	// The bottom is the last screenful, not the last line: scrolling until one
	// row sits above an empty screen is a text box's behaviour, not a pager's.
	CHECK(view.Top == 90);

	view.Apply(Key::Down, 100, 10);
	CHECK(view.Top == 90);

	view.Apply(Key::Home, 100, 10);
	CHECK(view.Top == 0);
}

TEST_CASE("a page keeps one line of what was being read", "[cdn][terminal]") {
	Viewport view;
	view.Apply(Key::PageDown, 100, 10);
	CHECK(view.Top == 9);

	view.Apply(Key::PageUp, 100, 10);
	CHECK(view.Top == 0);

	// A screen with no room to overlap still moves.
	Viewport tiny;
	tiny.Apply(Key::PageDown, 100, 1);
	CHECK(tiny.Top == 1);
}

TEST_CASE("a document shorter than the screen does not scroll", "[cdn][terminal]") {
	Viewport view;
	view.Apply(Key::PageDown, 4, 20);
	CHECK(view.Top == 0);
	view.Apply(Key::End, 4, 20);
	CHECK(view.Top == 0);
}

TEST_CASE("trimming cuts on a character rather than on a byte", "[cdn][terminal]") {
	CHECK(TrimToColumns("abcdef", 3) == "abc");
	CHECK(TrimToColumns("abc", 10) == "abc");
	CHECK(TrimToColumns("", 10).empty());

	// A sparkline is block glyphs and an asset's name is whatever an author
	// typed, so both reach the right-hand edge as UTF-8. Half a codepoint is a
	// replacement box on most terminals and an eaten neighbour on some.
	CHECK(TrimToColumns("▁▂▃", 2) == "▁▂");
	CHECK(TrimToColumns("▁▂▃", 3) == "▁▂▃");
	CHECK(TrimToColumns("▁▂▃", 0).empty());
	CHECK(TrimToColumns("é", 1) == "é");
}

TEST_CASE("a frame fills the screen it was given", "[cdn][terminal]") {
	const Tree tree;
	const Publication publication = Content(tree);
	const Dashboard board(publication, StoreFootprint{}, "127.0.0.1:9080");

	const Viewport view;
	const std::string frame = RenderFrame(board, view, ScreenSize{80, 24});

	// Twenty-three document rows and a status bar, which is the whole of the
	// screen and no more - a frame that overran would scroll the terminal and
	// leave the top row somewhere above it.
	CHECK(CountLines(frame) == 23);
	CHECK(frame.starts_with("\x1b[H"));
	CHECK(frame.find("127.0.0.1:9080") != std::string::npos);
	CHECK(frame.find("q quit") != std::string::npos);
	CHECK(frame.find("of " + std::to_string(board.Lines())) != std::string::npos);
}

TEST_CASE("a frame draws where the view is scrolled to", "[cdn][terminal]") {
	const Tree tree;
	const Publication publication = Content(tree);
	const Dashboard board(publication, StoreFootprint{}, "test");

	Viewport view;
	view.Apply(Key::End, board.Lines(), 23);
	const std::string frame = RenderFrame(board, view, ScreenSize{80, 24});

	// The listing is largest first, so the smallest asset is the document's
	// last line - and the title, which is its first, is nowhere on the screen.
	CHECK(frame.find("meshes/asset-0.mesh") != std::string::npos);
	CHECK(frame.find("content origin") == std::string::npos);
}

TEST_CASE("a narrow screen cuts the line rather than wrapping it", "[cdn][terminal]") {
	const Tree tree;
	const Publication publication = Content(tree);
	const Dashboard board(publication, StoreFootprint{}, "127.0.0.1:9080");

	const Viewport view;
	const std::string frame = RenderFrame(board, view, ScreenSize{20, 6});

	// A wrapped line takes two rows, and a screen of six rows would then hold
	// four of them - so what is drawn no longer matches what was scrolled to.
	CHECK(CountLines(frame) == 5);
	CHECK(frame.find("127.0.0.1:9080") == std::string::npos);
}
