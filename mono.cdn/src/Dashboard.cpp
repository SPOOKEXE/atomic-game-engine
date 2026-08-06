#include <engine/assets/AssetKind.hpp>

#include <algorithm>
#include <array>
#include <cdn/Dashboard.hpp>
#include <cstdio>
#include <map>
#include <utility>

// The layout, and the one thing about it that is load-bearing: **the live
// section has a fixed line count**. It is rewritten on every sample, and a
// section that grew a row when a counter became interesting would move
// everything below it — so an operator reading the asset list would find it
// sliding under the cursor for reasons nothing on screen explains.

namespace cdn {
	namespace {
		using engine::assets::AssetEntry;
		using engine::assets::AssetKind;

		// The scale a byte count is read at. Powers of 1024 under the decimal
		// names every other tool an operator has open uses.
		constexpr uint64_t KILOBYTE = 1024;
		constexpr uint64_t MEGABYTE = KILOBYTE * 1024;
		constexpr uint64_t GIGABYTE = MEGABYTE * 1024;
		constexpr uint64_t TERABYTE = GIGABYTE * 1024;

		// The bar heights, low to high. Eight glyphs and a floor, because that
		// is what the block-element range gives and a ninth would have to be
		// invented.
		constexpr std::array<std::string_view, 9> BARS = {"·", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};

		// How wide the columns of a listing are. Named rather than repeated at
		// each `Pad` call, so a change to one column is a change to one line.
		constexpr size_t BYTES_COLUMN = 10;
		constexpr size_t KIND_COLUMN = 9;

		std::string Text(const char *format, auto... arguments) {
			std::array<char, 256> buffer{};
			const int written = std::snprintf(buffer.data(), buffer.size(), format, arguments...);
			return written > 0 ? std::string(buffer.data(), static_cast<size_t>(written)) : std::string();
		}

		// Right-aligns into a column, and lets an over-long value push the
		// column rather than being cut — a truncated size is a wrong size.
		std::string Right(std::string_view value, size_t columns) {
			if (value.size() >= columns) {
				return std::string(value);
			}
			return std::string(columns - value.size(), ' ') + std::string(value);
		}

		std::string Left(std::string_view value, size_t columns) {
			std::string padded(value);
			if (padded.size() < columns) {
				padded.append(columns - padded.size(), ' ');
			}
			return padded;
		}

		// Counters only rise, but a caller may hand over a service that was
		// restarted underneath. A backwards step is zero rather than a number
		// near 2^64.
		uint64_t Since(uint64_t total, uint64_t previous) {
			return total > previous ? total - previous : 0;
		}

		DashboardLine Heading(std::string text) {
			return {std::move(text), LineStyle::Heading};
		}

		DashboardLine Row(std::string text) {
			return {std::move(text), LineStyle::Row};
		}

		DashboardLine Blank() {
			return {std::string(), LineStyle::Blank};
		}

		// One asset's row: size, kind, name. Size first because the column an
		// operator scans is the one that has to be aligned, and a name is the
		// field that varies in length.
		std::string AssetRow(const AssetEntry &asset) {
			return "  " + Right(FormatBytes(asset.TotalBytes), BYTES_COLUMN) + "  " +
				   Left(engine::assets::Describe(asset.Kind), KIND_COLUMN) + "  " + asset.Name;
		}
	}

	std::string FormatBytes(uint64_t bytes) {
		if (bytes >= TERABYTE) {
			return Text("%.1f TB", static_cast<double>(bytes) / static_cast<double>(TERABYTE));
		}
		if (bytes >= GIGABYTE) {
			return Text("%.1f GB", static_cast<double>(bytes) / static_cast<double>(GIGABYTE));
		}
		if (bytes >= MEGABYTE) {
			return Text("%.1f MB", static_cast<double>(bytes) / static_cast<double>(MEGABYTE));
		}
		if (bytes >= KILOBYTE) {
			return Text("%.1f KB", static_cast<double>(bytes) / static_cast<double>(KILOBYTE));
		}
		return Text("%llu B", static_cast<unsigned long long>(bytes));
	}

	std::string FormatRate(double bytesPerSecond) {
		if (!(bytesPerSecond > 0.0)) {
			// Also the NaN case, which a zero-length rate window would produce
			// and which prints as "-nan/s" if it is allowed through.
			return "0 B/s";
		}
		if (bytesPerSecond >= static_cast<double>(GIGABYTE)) {
			return Text("%.1f GB/s", bytesPerSecond / static_cast<double>(GIGABYTE));
		}
		if (bytesPerSecond >= static_cast<double>(MEGABYTE)) {
			return Text("%.1f MB/s", bytesPerSecond / static_cast<double>(MEGABYTE));
		}
		if (bytesPerSecond >= static_cast<double>(KILOBYTE)) {
			return Text("%.1f KB/s", bytesPerSecond / static_cast<double>(KILOBYTE));
		}
		return Text("%.0f B/s", bytesPerSecond);
	}

	std::string_view SparkGlyph(uint64_t value, uint64_t peak) {
		if (peak == 0 || value == 0) {
			return BARS.front();
		}
		// Scaled against the peak of its own row rather than a shared one: an
		// origin sends thousands of times what it receives, and one scale for
		// both draws the receive row flat forever.
		const uint64_t step = (value * (BARS.size() - 1) + peak - 1) / peak;
		return BARS[static_cast<size_t>(std::min<uint64_t>(step, BARS.size() - 1))];
	}

	Dashboard::Dashboard(
		const Publication &publication, const StoreFootprint &store, std::string_view endpoint
	)
		: Listening(endpoint) {
		const engine::assets::Manifest &manifest = publication.Contents();

		uint64_t contentBytes = 0;
		for (const AssetEntry &asset : manifest.Assets()) {
			contentBytes += asset.TotalBytes;
		}

		Content.push_back(Heading("CONTENT"));
		Content.push_back(
			Row(Text("  %zu assets in %zu bundles", manifest.Assets().size(), manifest.Bundles().size()))
		);
		Content.push_back(Row("  " + FormatBytes(contentBytes) + " of content, uncompressed"));
		Content.push_back(Row(Text(
			"  %s on disk in %llu chunks",
			FormatBytes(store.Bytes).c_str(),
			static_cast<unsigned long long>(store.Chunks)
		)));
		Content.push_back(Blank());

		// Keyed by the kinds the content actually has rather than by a list of
		// every kind there is. A second list of the kinds would be the thing
		// that is not updated the day one is appended to `AssetKind`.
		std::map<AssetKind, std::pair<uint64_t, uint64_t>> byKind;
		for (const AssetEntry &asset : manifest.Assets()) {
			auto &tally = byKind[asset.Kind];
			tally.first += 1;
			tally.second += asset.TotalBytes;
		}

		Content.push_back(Heading("BY KIND"));
		std::vector<std::pair<AssetKind, std::pair<uint64_t, uint64_t>>> kinds(byKind.begin(), byKind.end());
		std::sort(kinds.begin(), kinds.end(), [](const auto &left, const auto &right) {
			if (left.second.second != right.second.second) {
				return left.second.second > right.second.second;
			}
			return left.first < right.first;
		});
		for (const auto &[kind, tally] : kinds) {
			Content.push_back(
				Row("  " + Left(engine::assets::Describe(kind), KIND_COLUMN) + "  " +
					Right(Text("%llu", static_cast<unsigned long long>(tally.first)), 7) + " assets  " +
					Right(FormatBytes(tally.second), BYTES_COLUMN))
			);
		}
		if (kinds.empty()) {
			Content.push_back(Row("  nothing published"));
		}
		Content.push_back(Blank());

		std::vector<const AssetEntry *> largest;
		largest.reserve(manifest.Assets().size());
		for (const AssetEntry &asset : manifest.Assets()) {
			largest.push_back(&asset);
		}
		// Largest first, and ties broken by name so two runs over one manifest
		// produce one ordering — the same reason `Grouper` sorts the way it
		// does.
		std::sort(largest.begin(), largest.end(), [](const AssetEntry *left, const AssetEntry *right) {
			if (left->TotalBytes != right->TotalBytes) {
				return left->TotalBytes > right->TotalBytes;
			}
			return left->Name < right->Name;
		});

		Content.push_back(Heading(Text("LARGEST %zu", LARGEST_LISTED)));
		for (size_t index = 0; index < std::min(LARGEST_LISTED, largest.size()); ++index) {
			Content.push_back(Row(AssetRow(*largest[index])));
		}
		Content.push_back(Blank());

		Content.push_back(Heading(Text("ASSETS (%zu, largest first)", largest.size())));
		for (const AssetEntry *asset : largest) {
			Content.push_back(Row(AssetRow(*asset)));
		}
	}

	void Dashboard::Rotate(uint64_t nowMinute) {
		if (nowMinute <= NewestMinute) {
			// A clock that went backwards keeps filling the current bucket
			// rather than rewinding the ring. An hour of history thrown away
			// because something adjusted the wall clock is worse than a minute
			// that is slightly wide.
			return;
		}
		const uint64_t skipped = std::min<uint64_t>(nowMinute - NewestMinute, HISTORY_MINUTES);
		for (uint64_t step = 0; step < skipped; ++step) {
			NewestBucket = (NewestBucket + 1) % HISTORY_MINUTES;
			History[NewestBucket] = {};
		}
		NewestMinute = nowMinute;
	}

	void
	Dashboard::Sample(const ServiceCounters &counters, const CacheUsage &cache, uint64_t nowMilliseconds) {
		Held = cache;
		Latest = counters;

		const uint64_t minute = nowMilliseconds / 60'000;
		if (!Started) {
			// The first reading establishes the baseline and contributes no
			// traffic: whatever the service had already carried belongs to a
			// minute this ring has no bucket for, and putting it in the current
			// one would draw a spike that never happened.
			Started = true;
			Previous = counters;
			NewestMinute = minute;
			WindowStartMilliseconds = nowMilliseconds;
			LiveComposed = false;
			return;
		}

		Rotate(minute);

		const uint64_t sent = Since(counters.SentBytes, Previous.SentBytes);
		const uint64_t received = Since(counters.ReceivedBytes, Previous.ReceivedBytes);
		Previous = counters;

		History[NewestBucket].SentBytes += sent;
		History[NewestBucket].ReceivedBytes += received;
		WindowSentBytes += sent;
		WindowReceivedBytes += received;

		if (nowMilliseconds < WindowStartMilliseconds) {
			WindowStartMilliseconds = nowMilliseconds;
		} else if (nowMilliseconds - WindowStartMilliseconds >= RATE_WINDOW_MILLISECONDS) {
			const double seconds = static_cast<double>(nowMilliseconds - WindowStartMilliseconds) / 1000.0;
			SentRate = static_cast<double>(WindowSentBytes) / seconds;
			ReceivedRate = static_cast<double>(WindowReceivedBytes) / seconds;
			WindowSentBytes = 0;
			WindowReceivedBytes = 0;
			WindowStartMilliseconds = nowMilliseconds;
		}

		LiveComposed = false;
	}

	void Dashboard::ComposeLive() const {
		LiveComposed = true;

		const TrafficBucket hour = LastHour();

		uint64_t sentPeak = 0;
		uint64_t receivedPeak = 0;
		std::string sentBars;
		std::string receivedBars;
		for (size_t minute = 0; minute < HISTORY_MINUTES; ++minute) {
			const TrafficBucket bucket = MinuteAgo(minute);
			sentPeak = std::max(sentPeak, bucket.SentBytes);
			receivedPeak = std::max(receivedPeak, bucket.ReceivedBytes);
		}
		// Oldest on the left, so the line reads the way time does.
		for (size_t minute = HISTORY_MINUTES; minute > 0; --minute) {
			const TrafficBucket bucket = MinuteAgo(minute - 1);
			sentBars += SparkGlyph(bucket.SentBytes, sentPeak);
			receivedBars += SparkGlyph(bucket.ReceivedBytes, receivedPeak);
		}

		Live.clear();
		Live.push_back(
			Heading("atomic — content origin · " + (Listening.empty() ? "not listening" : Listening))
		);
		Live.push_back(Blank());

		Live.push_back(Heading("NETWORK"));
		Live.push_back(
			{"  now         out " + Right(FormatRate(SentRate), 11) + "   in " +
				 Right(FormatRate(ReceivedRate), 11),
			 LineStyle::Emphasis}
		);
		Live.push_back(
			Row("  last hour   out " + Right(FormatBytes(hour.SentBytes), 11) + "   in " +
				Right(FormatBytes(hour.ReceivedBytes), 11) + "   (60 minutes, the newest partial)")
		);
		Live.push_back(
			Row("  since start out " + Right(FormatBytes(Latest.SentBytes), 11) + "   in " +
				Right(FormatBytes(Latest.ReceivedBytes), 11))
		);
		Live.push_back(Row("  out " + sentBars + "  peak " + FormatBytes(sentPeak) + "/min"));
		Live.push_back(Row("  in  " + receivedBars + "  peak " + FormatBytes(receivedPeak) + "/min"));
		Live.push_back(Blank());

		Live.push_back(Heading("REQUESTS"));
		Live.push_back(Row(Text(
			"  bundles %llu · manifests %llu · dictionaries %llu · health %llu",
			static_cast<unsigned long long>(Latest.Bundles),
			static_cast<unsigned long long>(Latest.Manifests),
			static_cast<unsigned long long>(Latest.Dictionaries),
			static_cast<unsigned long long>(Latest.Health)
		)));
		Live.push_back(Row(Text(
			"  refused %llu · missing %llu · rejected %llu",
			static_cast<unsigned long long>(Latest.Refused),
			static_cast<unsigned long long>(Latest.Missing),
			static_cast<unsigned long long>(Latest.Rejected)
		)));
		Live.push_back(Row("  group payload served  " + FormatBytes(Latest.ServedBytes)));
		Live.push_back(Row(Text(
			"  prepared cache  %llu groups · %s of %s",
			static_cast<unsigned long long>(Held.Groups),
			FormatBytes(Held.Bytes).c_str(),
			FormatBytes(Held.CapacityBytes).c_str()
		)));
		Live.push_back(Blank());
	}

	size_t Dashboard::Lines() const {
		if (!LiveComposed) {
			ComposeLive();
		}
		return Live.size() + Content.size();
	}

	DashboardLine Dashboard::LineAt(size_t index) const {
		if (!LiveComposed) {
			ComposeLive();
		}
		if (index < Live.size()) {
			return Live[index];
		}
		const size_t row = index - Live.size();
		if (row < Content.size()) {
			return Content[row];
		}
		// Past the bottom is a blank line rather than a `Row` that happens to
		// be empty: a terminal styles what it is given, and an empty row drawn
		// as content is an empty row that gets a colour reset written into it.
		return Blank();
	}

	TrafficBucket Dashboard::MinuteAgo(size_t minutesAgo) const {
		if (minutesAgo >= HISTORY_MINUTES) {
			return {};
		}
		return History[(NewestBucket + HISTORY_MINUTES - minutesAgo) % HISTORY_MINUTES];
	}

	TrafficBucket Dashboard::LastHour() const {
		TrafficBucket total;
		for (const TrafficBucket &bucket : History) {
			total.SentBytes += bucket.SentBytes;
			total.ReceivedBytes += bucket.ReceivedBytes;
		}
		return total;
	}
}
