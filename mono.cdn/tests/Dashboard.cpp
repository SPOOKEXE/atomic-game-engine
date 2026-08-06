#include <engine/testing/Suite.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cdn/Dashboard.hpp>
#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

TEST_SUITE_ID("cdn.dashboard")
TEST_DEPENDS("cdn.origin")
TEST_DEPENDS("cdn.service")
TEST_DEPENDS("engine.assets.manifest")

using Catch::Approx;
using cdn::CacheUsage;
using cdn::ContentRoot;
using cdn::Dashboard;
using cdn::DashboardLine;
using cdn::FormatBytes;
using cdn::FormatRate;
using cdn::LineStyle;
using cdn::Publication;
using cdn::ServiceCounters;
using cdn::StoreFootprint;
using engine::assets::AssetKind;
using engine::assets::ChunkEntry;
using engine::assets::ContentHash;
using engine::assets::Hasher;
using engine::assets::Manifest;

namespace {
	namespace fs = std::filesystem;

	constexpr uint64_t MEGABYTE = 1024 * 1024;
	constexpr uint64_t MINUTE = 60'000;

	// A directory to mount. A publication holds a content root and nothing
	// here reads a file through it.
	struct Tree {
		fs::path Root;

		Tree() {
			static int serial = 0;
			Root = fs::temp_directory_path() / ("atomic-dashboard-" + std::to_string(++serial));
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

	// A chunk of a stated length. The bytes are never read here — the manifest
	// records the length and the dashboard adds those up.
	ChunkEntry Sized(std::string_view name, uint32_t bytes) {
		ChunkEntry entry;
		entry.Hash = Hasher::Of(
			std::span<const std::byte>(reinterpret_cast<const std::byte *>(name.data()), name.size())
		);
		entry.Bytes = bytes;
		return entry;
	}

	// Two meshes, one texture and one sound, with sizes far enough apart that
	// an ordering is unambiguous.
	Publication Content(const Tree &tree) {
		Manifest manifest;
		std::vector<ContentHash> assets;
		assets.push_back(manifest.AddAsset("meshes/city.mesh", AssetKind::Mesh, {Sized("city", 8 * 1024)}));
		assets.push_back(manifest.AddAsset("meshes/rock.mesh", AssetKind::Mesh, {Sized("rock", 2 * 1024)}));
		assets.push_back(manifest.AddAsset("textures/sky.png", AssetKind::Texture, {Sized("sky", 4 * 1024)}));
		assets.push_back(manifest.AddAsset("audio/wind.ogg", AssetKind::Audio, {Sized("wind", 1024)}));
		REQUIRE(manifest.AddBundle(assets).has_value());
		return Publication(tree.Mount(), std::move(manifest));
	}

	// The whole document as one string, for asserting that a row is present
	// without pinning where it is.
	std::string Text(const Dashboard &board) {
		std::string all;
		for (size_t line = 0; line < board.Lines(); ++line) {
			all += board.LineAt(line).Text;
			all += '\n';
		}
		return all;
	}

	size_t IndexOfLine(const Dashboard &board, std::string_view fragment) {
		for (size_t line = 0; line < board.Lines(); ++line) {
			if (board.LineAt(line).Text.find(fragment) != std::string::npos) {
				return line;
			}
		}
		return board.Lines();
	}
}

TEST_CASE("a byte count is formatted at the scale it is read at", "[cdn][dashboard]") {
	CHECK(FormatBytes(0) == "0 B");
	CHECK(FormatBytes(512) == "512 B");
	CHECK(FormatBytes(1536) == "1.5 KB");
	CHECK(FormatBytes(4 * MEGABYTE) == "4.0 MB");
	CHECK(FormatBytes(3ull * 1024 * MEGABYTE) == "3.0 GB");
	CHECK(FormatBytes(2ull * 1024 * 1024 * MEGABYTE) == "2.0 TB");
}

TEST_CASE("a rate picks its unit from the load", "[cdn][dashboard]") {
	// The whole of "MB/s or KB/s depending on load": an origin idling and an
	// origin saturating a link are four orders of magnitude apart, and one
	// fixed scale is unreadable at whichever end it was not chosen for.
	CHECK(FormatRate(0.0) == "0 B/s");
	CHECK(FormatRate(900.0) == "900 B/s");
	CHECK(FormatRate(1536.0) == "1.5 KB/s");
	CHECK(FormatRate(4.0 * static_cast<double>(MEGABYTE)) == "4.0 MB/s");

	// A rate window of zero length would divide by zero. Whatever comes of
	// that is not printed as a number.
	CHECK(FormatRate(-1.0) == "0 B/s");
}

TEST_CASE("a sparkline scales against its own peak", "[cdn][dashboard]") {
	CHECK(cdn::SparkGlyph(0, 100) == cdn::SparkGlyph(0, 0));
	CHECK(cdn::SparkGlyph(100, 100) == "█");
	CHECK(cdn::SparkGlyph(1, 100) != "█");

	// Nothing recorded draws the floor rather than dividing by the peak.
	CHECK(cdn::SparkGlyph(7, 0) == cdn::SparkGlyph(0, 1));
}

TEST_CASE("the content section counts what is published", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	const Dashboard board(publication, StoreFootprint{12 * 1024, 9}, "127.0.0.1:9080");

	const std::string text = Text(board);
	CHECK(text.find("4 assets in 1 bundles") != std::string::npos);
	CHECK(text.find("15.0 KB of content") != std::string::npos);
	CHECK(text.find("12.0 KB on disk in 9 chunks") != std::string::npos);
	CHECK(text.find("127.0.0.1:9080") != std::string::npos);
}

TEST_CASE("the breakdown is per kind and covers only the kinds present", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	const Dashboard board(publication, StoreFootprint{}, "test");

	const std::string text = Text(board);
	// Two meshes at 10 KB together, one texture, one sound — and no row for a
	// kind this content does not have, because the breakdown is built from the
	// manifest rather than from a second list of every kind there is.
	CHECK(text.find("mesh") != std::string::npos);
	CHECK(text.find("2 assets") != std::string::npos);
	CHECK(text.find("texture") != std::string::npos);
	CHECK(text.find("audio") != std::string::npos);
	CHECK(text.find("script") == std::string::npos);
	CHECK(text.find("video") == std::string::npos);
}

TEST_CASE("the largest assets are listed first, and so is every other one", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	const Dashboard board(publication, StoreFootprint{}, "test");

	const size_t largest = IndexOfLine(board, "LARGEST");
	const size_t everything = IndexOfLine(board, "ASSETS (4");
	REQUIRE(largest < board.Lines());
	REQUIRE(everything < board.Lines());

	// The five heaviest, then every asset, both heaviest first. Four assets
	// here, so the top-five section is short by one rather than padded.
	CHECK(board.LineAt(largest + 1).Text.find("meshes/city.mesh") != std::string::npos);
	CHECK(board.LineAt(largest + 4).Text.find("audio/wind.ogg") != std::string::npos);
	CHECK(board.LineAt(everything + 1).Text.find("meshes/city.mesh") != std::string::npos);
	CHECK(board.LineAt(everything + 4).Text.find("audio/wind.ogg") != std::string::npos);
	CHECK(board.LineAt(everything + 5).Text.empty());
}

TEST_CASE("a line past the end is empty rather than out of bounds", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	const Dashboard board(publication, StoreFootprint{}, "test");

	// What a viewport scrolled to the bottom of a short document asks for.
	const DashboardLine past = board.LineAt(board.Lines() + 100);
	CHECK(past.Text.empty());
	CHECK(past.Style == LineStyle::Blank);
}

TEST_CASE("the first sample is a baseline and draws no traffic", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	// A service that had already carried a gigabyte before the dashboard was
	// opened. That gigabyte belongs to minutes this ring has no bucket for, and
	// charging it to the current one would draw a spike nothing caused.
	ServiceCounters counters;
	counters.SentBytes = 1024 * MEGABYTE;
	counters.ReceivedBytes = MEGABYTE;
	board.Sample(counters, CacheUsage{}, 0);

	CHECK(board.LastHour().SentBytes == 0);
	CHECK(board.SentBytesPerSecond() == Approx(0.0));
}

TEST_CASE("a rate is measured over its window", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	ServiceCounters counters;
	board.Sample(counters, CacheUsage{}, 0);

	counters.SentBytes = 2 * MEGABYTE;
	counters.ReceivedBytes = 4096;
	board.Sample(counters, CacheUsage{}, Dashboard::RATE_WINDOW_MILLISECONDS);

	CHECK(board.SentBytesPerSecond() == Approx(2.0 * static_cast<double>(MEGABYTE)));
	CHECK(board.ReceivedBytesPerSecond() == Approx(4096.0));
	CHECK(Text(board).find("2.0 MB/s") != std::string::npos);
}

TEST_CASE("a rate is not published until its window has run", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	ServiceCounters counters;
	board.Sample(counters, CacheUsage{}, 0);

	// Ten milliseconds apart is the serving loop's own cadence. A rate taken
	// over one of those swings by a factor of a hundred between redraws, which
	// is why the window exists — but the traffic is still recorded.
	counters.SentBytes = 64 * 1024;
	board.Sample(counters, CacheUsage{}, 10);

	CHECK(board.SentBytesPerSecond() == Approx(0.0));
	CHECK(board.LastHour().SentBytes == 64 * 1024);
}

TEST_CASE("traffic lands in the minute it arrived in", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	ServiceCounters counters;
	board.Sample(counters, CacheUsage{}, 0);

	counters.SentBytes = 1000;
	board.Sample(counters, CacheUsage{}, 30'000);

	counters.SentBytes = 3000;
	board.Sample(counters, CacheUsage{}, MINUTE + 10'000);

	CHECK(board.MinuteAgo(0).SentBytes == 2000);
	CHECK(board.MinuteAgo(1).SentBytes == 1000);
	CHECK(board.LastHour().SentBytes == 3000);
}

TEST_CASE("an hour of history ages out and nothing older is kept", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	ServiceCounters counters;
	board.Sample(counters, CacheUsage{}, 0);

	counters.SentBytes = 5000;
	board.Sample(counters, CacheUsage{}, 1000);
	CHECK(board.LastHour().SentBytes == 5000);

	// A full ring later. The bucket that held it is the one being written now,
	// so an hour-old value must have been cleared on the way past rather than
	// added to.
	board.Sample(counters, CacheUsage{}, Dashboard::HISTORY_MINUTES * MINUTE);
	CHECK(board.LastHour().SentBytes == 0);
	CHECK(board.MinuteAgo(0).SentBytes == 0);
}

TEST_CASE("a quiet gap clears the minutes it crossed", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	ServiceCounters counters;
	board.Sample(counters, CacheUsage{}, 0);
	counters.SentBytes = 7000;
	board.Sample(counters, CacheUsage{}, 1000);

	counters.SentBytes = 8000;
	board.Sample(counters, CacheUsage{}, 5 * MINUTE);

	CHECK(board.MinuteAgo(0).SentBytes == 1000);
	CHECK(board.MinuteAgo(1).SentBytes == 0);
	CHECK(board.MinuteAgo(4).SentBytes == 0);
	CHECK(board.MinuteAgo(5).SentBytes == 7000);
}

TEST_CASE("a clock that steps backwards does not rewind the ring", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	ServiceCounters counters;
	board.Sample(counters, CacheUsage{}, 10 * MINUTE);
	counters.SentBytes = 2000;
	board.Sample(counters, CacheUsage{}, 10 * MINUTE + 1000);

	// An hour of history thrown away because something adjusted a clock is
	// worse than a minute that is slightly wide.
	counters.SentBytes = 3000;
	board.Sample(counters, CacheUsage{}, 2 * MINUTE);
	CHECK(board.MinuteAgo(0).SentBytes == 3000);
	CHECK(board.LastHour().SentBytes == 3000);
}

TEST_CASE("a counter that went backwards contributes nothing", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	ServiceCounters counters;
	counters.SentBytes = 9000;
	board.Sample(counters, CacheUsage{}, 0);

	counters.SentBytes = 10;
	board.Sample(counters, CacheUsage{}, 1000);

	CHECK(board.LastHour().SentBytes == 0);
}

TEST_CASE("the live section keeps its line count", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	// The load-bearing property of the layout: a section that grew a row when a
	// counter became interesting would slide the asset list under whoever was
	// reading it, for a reason nothing on screen explains.
	const size_t quiet = board.Lines();
	const size_t assets = IndexOfLine(board, "ASSETS (4");

	ServiceCounters counters;
	board.Sample(counters, CacheUsage{}, 0);
	counters.SentBytes = 900 * MEGABYTE;
	counters.ReceivedBytes = 3 * MEGABYTE;
	counters.Bundles = 4000;
	counters.Refused = 12;
	board.Sample(
		counters, CacheUsage{64 * MEGABYTE, 3, 256 * MEGABYTE}, 4 * Dashboard::RATE_WINDOW_MILLISECONDS
	);

	CHECK(board.Lines() == quiet);
	CHECK(IndexOfLine(board, "ASSETS (4") == assets);
}

TEST_CASE("the live section reports requests and the cache", "[cdn][dashboard]") {
	const Tree tree;
	const Publication publication = Content(tree);
	Dashboard board(publication, StoreFootprint{}, "test");

	ServiceCounters counters;
	counters.Bundles = 7;
	counters.Manifests = 2;
	counters.Refused = 1;
	counters.ServedBytes = 3 * MEGABYTE;
	counters.SentBytes = 4 * MEGABYTE;
	board.Sample(counters, CacheUsage{2 * MEGABYTE, 5, 256 * MEGABYTE}, 0);

	const std::string text = Text(board);
	CHECK(text.find("bundles 7") != std::string::npos);
	CHECK(text.find("refused 1") != std::string::npos);
	// The group payload and what the interface carried are different
	// measurements and are shown as two.
	CHECK(text.find("group payload served  3.0 MB") != std::string::npos);
	CHECK(text.find("4.0 MB") != std::string::npos);
	CHECK(text.find("5 groups · 2.0 MB of 256.0 MB") != std::string::npos);
}
