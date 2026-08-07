// What is moving between this editor and the origins it is configured against.
//
// **The editor had the configuration and never used it.** `ContentSources` was
// saved, loaded and edited since v0.9, and nothing in the studio ever built a
// `delivery::AssetClient` from it — so a publisher key could be wrong, an origin
// could be down and an address could be a host name that never resolves, and the
// preferences page would look exactly the same either way. This file is the half
// that makes the settings do something and then says what happened.
//
// **A model over counters, and no clock of its own.** `cdn::Dashboard` is the
// same shape on the origin's side and for the same reasons — everything here is
// arithmetic over `DeliveryCounters`, `UploadCounters` and a ring of one-second
// samples, and every sample is stamped with a time passed in. What it does *not*
// share with the dashboard is a text model: an origin's operator is looking at a
// terminal and this is an imgui table, so the layout is drawn directly and the
// thing worth sharing was the arithmetic rather than the lines.
//
// **The rate is measured and not derived.** "Bytes over the run divided by how
// long the editor has been open" is a number that only falls, and is useless the
// moment somebody wants to know whether a download is moving *now*. So the
// samples are a ring of the last few seconds and the rate is what crossed inside
// it — which is the same decision `Dashboard`'s minute buckets make, at the
// resolution an interactive panel is read at.

#include <engine/core/Log.hpp>
#include <engine/delivery/Client.hpp>
#include <engine/delivery/Uploader.hpp>
#include <engine/ui/Theme.hpp>

#include <algorithm>
#include <cdn/LocalStore.hpp>
#include <filesystem>
#include <fstream>
#include <imgui.h>
#include <studio/Editor.hpp>
#include <studio/Widgets.hpp>
#include <system_error>

namespace studio {

	namespace {
		// A byte count somebody can read at a glance.
		//
		// **Powers of 1024 under decimal names**, matching `cdn::Readable` and
		// every tool an operator already has open. Being right about the prefix
		// and alone in it helps nobody comparing this against `df`.
		std::string Readable(uint64_t bytes) {
			static const char *UNITS[] = {"B", "KB", "MB", "GB", "TB"};

			double scaled = static_cast<double>(bytes);
			size_t unit = 0;
			while (scaled >= 1024.0 && unit + 1 < std::size(UNITS)) {
				scaled /= 1024.0;
				unit++;
			}

			char text[32];
			std::snprintf(text, sizeof(text), unit == 0 ? "%.0f %s" : "%.1f %s", scaled, UNITS[unit]);
			return text;
		}

		std::string PerSecond(double bytes) {
			return Readable(static_cast<uint64_t>(std::max(0.0, bytes))) + "/s";
		}

		// One labelled number in the two-column table both halves use.
		void Row(const char *label, const std::string &value, const char *note = nullptr) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(label);
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(value.c_str());
			if (note != nullptr && ImGui::IsItemHovered()) {
				ImGui::SetTooltip("%s", note);
			}
		}
	}

	void NetworkSamples::Observe(double nowSeconds, uint64_t down, uint64_t up) {
		if (Filled == 0) {
			// **The first observation seeds and does not measure.** A ring whose
			// first sample counted every byte since the process started would
			// show one enormous spike on the frame the panel is first opened,
			// which is exactly when somebody is deciding whether to believe it.
			At = 0;
			Filled = 1;
			Points[0] = Sample{.Seconds = nowSeconds, .Down = down, .Up = up};
			return;
		}

		const Sample &last = Points[At];
		if (nowSeconds - last.Seconds < INTERVAL) {
			return;
		}

		At = (At + 1) % CAPACITY;
		Points[At] = Sample{.Seconds = nowSeconds, .Down = down, .Up = up};
		Filled = std::min<size_t>(Filled + 1, CAPACITY);
	}

	NetworkRates NetworkSamples::Rates() const {
		if (Filled < 2) {
			return {};
		}

		// The oldest sample still in the ring against the newest. Not the last
		// two: a pair one interval apart is one frame's noise, and what the
		// panel is being read for is whether a transfer is moving.
		const size_t oldest = (At + CAPACITY - (Filled - 1)) % CAPACITY;
		const Sample &first = Points[oldest];
		const Sample &last = Points[At];

		const double elapsed = last.Seconds - first.Seconds;
		if (elapsed <= 0.0) {
			return {};
		}

		NetworkRates rates;
		rates.DownPerSecond = static_cast<double>(last.Down - first.Down) / elapsed;
		rates.UpPerSecond = static_cast<double>(last.Up - first.Up) / elapsed;
		rates.WindowSeconds = elapsed;
		return rates;
	}

	void Editor::RebuildContentClients() {
		// **Both are rebuilt together and from one settings block**, because
		// they read one list: a role edited in the preferences page changes
		// which of them a row belongs to, and rebuilding one would leave the
		// other holding the previous answer.
		const engine::delivery::DeliverySettings settings = Content.ToSettings();

		ContentClient.reset();
		ContentUploads.reset();
		ContentSamples = NetworkSamples{};

		if (settings.IsValid()) {
			ContentClient = engine::delivery::MakeAssetClient(settings);
		} else {
			// Said once, here, rather than as a failed fetch later. The two
			// reasons are worth separating because they are fixed on different
			// pages: no key is a trust problem and no source is an address one.
			ContentStatus = Content.PublisherKey.empty()
								? "no publisher key — nothing can be fetched, because nothing could be verified"
								: "no usable read source — check the addresses and that a row is enabled";
		}

		// **Built even when delivery is not.** An uploader verifies nothing, so
		// it does not need a publisher key — and an editor being used to *seed*
		// an origin is exactly the case where no manifest has been signed yet
		// and `DeliverySettings::IsValid` is false.
		ContentUploads = engine::delivery::MakeUploader(settings);
	}

	void Editor::PumpContent(double frameSeconds) {
		ContentSeconds += frameSeconds;

		if (ContentClient) {
			ContentClient->Pump();
		}

		if (ContentUploads) {
			ContentUploads->Pump();
			for (const engine::delivery::UploadOutcome &outcome : ContentUploads->Take()) {
				if (!outcome.Delivered) {
					// **Failures are logged and successes are not.** A tree of
					// three hundred files would otherwise be three hundred log
					// lines nobody reads; the counters say how it went and the
					// log says what went wrong.
					ENGINE_WARN(
						"upload: {} → {}: {}",
						outcome.File.filename().string(),
						outcome.Destination,
						outcome.Detail
					);
					UploadFailures++;
				}
			}

			if (ContentUploads->Remaining() == 0 && UploadQueued > 0) {
				const engine::delivery::UploadCounters &counters = ContentUploads->Counters();
				ContentStatus = std::to_string(counters.Stored) + " stored, " +
								std::to_string(counters.Skipped) + " already there, " +
								std::to_string(counters.Refused + counters.Failed) + " failed";
				ENGINE_INFO("upload: {}", ContentStatus);
				UploadQueued = 0;
			}
		}

		uint64_t down = 0;
		if (ContentClient) {
			down = ContentClient->Counters().TransferredBytes;
		}
		const uint64_t up = ContentUploads ? ContentUploads->Counters().SentBytes : 0;
		ContentSamples.Observe(ContentSeconds, down, up);
	}

	void Editor::UploadStore() {
		if (!ContentUploads) {
			ContentStatus = "no write source — give a row the write role and an ingest key";
			return;
		}

		// **`raw/` and not `processed/`.** What an origin's inbox wants is
		// content, and `processed/` is a *published* store — chunks under hash
		// names plus a signed manifest, which would arrive as several thousand
		// unrecognisable files. The far end publishes what it receives; sending
		// it something already published would be publishing twice.
		const cdn::LocalPaths paths = cdn::DefaultLocalPaths();

		std::error_code failure;
		if (!std::filesystem::is_directory(paths.Raw, failure)) {
			ContentStatus = "nothing to upload — the content store is empty";
			return;
		}

		size_t queued = 0;
		for (std::filesystem::recursive_directory_iterator walk(
				 paths.Raw, std::filesystem::directory_options::skip_permission_denied, failure
			 );
			 walk != std::filesystem::recursive_directory_iterator();
			 walk.increment(failure)) {
			if (failure) {
				break;
			}
			if (walk->is_regular_file(failure) && ContentUploads->Add(walk->path())) {
				queued++;
			}
		}

		UploadQueued = queued;
		UploadFailures = 0;
		ContentStatus = std::to_string(queued) + " file(s) queued to " +
						std::to_string(ContentUploads->Destinations().size()) + " destination(s)";
		ENGINE_INFO("upload: {}", ContentStatus);
	}

	void Editor::DownloadAsset(const std::string &name) {
		if (!ContentClient) {
			ContentStatus = "delivery is not configured — see the Content page in Preferences";
			return;
		}
		if (name.empty()) {
			return;
		}

		const engine::delivery::RequestId id = ContentClient->Request(name);
		if (!id.IsValid()) {
			ContentStatus = "could not ask for " + name;
			return;
		}

		Downloads.push_back(PendingDownload{.Name = name, .Id = id});
		ContentStatus = "asked for " + name;
	}

	void Editor::CollectDownloads() {
		if (!ContentClient) {
			Downloads.clear();
			return;
		}

		for (size_t index = 0; index < Downloads.size();) {
			PendingDownload &pending = Downloads[index];
			const engine::delivery::RequestState state = ContentClient->StateOf(pending.Id);

			if (state == engine::delivery::RequestState::Pending) {
				index++;
				continue;
			}

			if (state == engine::delivery::RequestState::Ready) {
				std::optional<engine::delivery::Asset> asset = ContentClient->Take(pending.Id);
				if (asset) {
					// **Into the local store, under the hash of what arrived.**
					// A download that landed somewhere else would be a second
					// place content lives, and the whole reason the store is
					// content-addressed is that there is one.
					const cdn::LocalPaths paths = cdn::DefaultLocalPaths();
					if (cdn::EnsureLocalStore(paths)) {
						const std::filesystem::path stored =
							paths.Raw / (asset->Root.ToHex() + std::filesystem::path(asset->Name).extension().string());
						std::ofstream out(stored, std::ios::binary | std::ios::trunc);
						out.write(
							reinterpret_cast<const char *>(asset->Bytes.data()),
							static_cast<std::streamsize>(asset->Bytes.size())
						);
						ContentStatus = out.good()
											? pending.Name + " — " + Readable(asset->Bytes.size()) + " into the store"
											: pending.Name + " — could not be written";
					}
				}
			} else {
				ContentStatus = pending.Name + " — every source was tried and none answered";
			}

			Downloads.erase(Downloads.begin() + static_cast<ptrdiff_t>(index));
		}
	}

	void Editor::DrawNetwork() {
		if (!ShowNetwork) {
			return;
		}

		if (!ImGui::Begin("Network", &ShowNetwork)) {
			ImGui::End();
			return;
		}

		CollectDownloads();

		const NetworkRates rates = ContentSamples.Rates();

		// --- what is moving right now -----------------------------------------
		//
		// First, and emphasised, because it is the one row somebody opens this
		// panel to see. Everything below it is a total, and a total cannot say
		// whether a transfer is progressing.
		ImGui::SeparatorText("Right now");

		if (ImGui::BeginTable("##rates", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch, 0.45f);
			ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);

			Row("Down", PerSecond(rates.DownPerSecond), "compressed bytes off the wire, over the last few seconds");
			Row("Up", PerSecond(rates.UpPerSecond), "bytes sent to write origins");

			if (ContentClient) {
				Row("In flight", std::to_string(ContentClient->Outstanding()) + " request(s)");
			}
			if (ContentUploads) {
				Row("Queued", std::to_string(ContentUploads->Remaining()) + " upload(s)");
			}
			ImGui::EndTable();
		}

		if (rates.WindowSeconds <= 0.0) {
			ImGui::TextDisabled("no window yet — rates appear after a second of samples");
		}

		// --- downloading ------------------------------------------------------

		ImGui::SeparatorText("Downloading");

		if (!ContentClient) {
			ImGui::TextColored(
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				"not configured — Preferences > Content needs a publisher key and a readable source"
			);
		} else {
			const engine::delivery::DeliveryCounters &counters = ContentClient->Counters();

			if (ImGui::BeginTable("##down", 2, ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch, 0.45f);
				ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);

				Row("Manifest", ContentClient->Ready() ? "verified" : "waiting");
				Row("Cache hits", std::to_string(counters.CacheHits), "assets served without touching a source");
				Row("Cache misses", std::to_string(counters.CacheMisses));
				Row("Bundles", std::to_string(counters.Bundles));
				Row("Transferred", Readable(counters.TransferredBytes), "as it travelled — compressed");
				Row("Expanded", Readable(counters.ExpandedBytes), "what those became");

				// **The pair is what answers 'did this travel compressed'**, and
				// it is a question about the wire — so it is measured at it
				// rather than inferred from a setting. See `DeliveryCounters`.
				if (counters.TransferredBytes > 0) {
					char ratio[32];
					std::snprintf(
						ratio,
						sizeof(ratio),
						"%.2fx",
						static_cast<double>(counters.ExpandedBytes) /
							static_cast<double>(counters.TransferredBytes)
					);
					Row("Compression", ratio);
				}

				Row("Source failures", std::to_string(counters.SourceFailures), "times a source was passed over");

				ImGui::EndTable();
			}

			// **Verification failures get their own line and a colour**, because
			// they are not an operational event. A source that is down is
			// ordinary; a source serving bytes that do not match a signed root
			// is corruption or an attack, and burying it in a table of totals is
			// how it gets missed.
			if (counters.VerificationFailures > 0) {
				ImGui::TextColored(
					ImVec4(0.95f, 0.45f, 0.35f, 1.0f),
					"%llu payload(s) did not verify against the signed manifest",
					static_cast<unsigned long long>(counters.VerificationFailures)
				);
			}

			ImGui::SetNextItemWidth(-90.0f);
			ImGui::InputTextWithHint(
				"##fetch", "an asset name from the manifest", DownloadName, sizeof(DownloadName)
			);
			ImGui::SameLine();
			ImGui::BeginDisabled(DownloadName[0] == '\0');
			if (ImGui::Button("Fetch", ImVec2(80.0f, 0.0f))) {
				DownloadAsset(DownloadName);
			}
			ImGui::EndDisabled();

			for (const PendingDownload &pending : Downloads) {
				ImGui::BulletText("%s — waiting", pending.Name.c_str());
			}
		}

		// --- uploading --------------------------------------------------------

		ImGui::SeparatorText("Uploading");

		if (!ContentUploads) {
			ImGui::TextColored(
				ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled),
				"no write source — give a row the write role and an ingest key in Preferences > Content"
			);
		} else {
			const engine::delivery::UploadCounters &counters = ContentUploads->Counters();

			if (ImGui::BeginTable("##up", 2, ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("##what", ImGuiTableColumnFlags_WidthStretch, 0.45f);
				ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch, 0.55f);

				Row("Destinations", std::to_string(ContentUploads->Destinations().size()));
				Row("Stored", std::to_string(counters.Stored));
				Row("Already there", std::to_string(counters.Skipped), "the probe that makes a re-upload cheap");
				Row("Sent", Readable(counters.SentBytes));
				Row("Refused", std::to_string(counters.Refused), "wrong key, or an origin that takes no writes");
				Row("Failed", std::to_string(counters.Failed));
				ImGui::EndTable();
			}

			for (const engine::delivery::Source &target : ContentUploads->Destinations()) {
				ImGui::BulletText("%s — %s", target.Name.c_str(), target.Location.c_str());
			}

			ImGui::BeginDisabled(ContentUploads->Remaining() > 0);
			if (ImGui::Button("Upload the content store")) {
				UploadStore();
			}
			ImGui::EndDisabled();

			if (ContentUploads->Remaining() > 0 && UploadQueued > 0) {
				const size_t done = UploadQueued > ContentUploads->Remaining() ? UploadQueued - ContentUploads->Remaining() : 0;
				ImGui::SameLine();
				ImGui::Text("%zu / %zu", done, UploadQueued);
			}
		}

		if (!ContentStatus.empty()) {
			ImGui::Separator();
			ImGui::TextWrapped("%s", ContentStatus.c_str());
		}

		ImGui::End();
	}
}
