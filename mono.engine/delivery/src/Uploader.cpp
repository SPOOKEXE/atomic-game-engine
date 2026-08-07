#include <engine/assets/ContentHash.hpp>
#include <engine/core/Log.hpp>
#include <engine/delivery/Uploader.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Client.hpp>

#include <fstream>
#include <system_error>
#include <utility>

namespace engine::delivery {
	namespace {
		namespace http = net::http;

		// A destination, resolved once at construction.
		struct Destination {
			Source Descriptor;

			// For `SourceKind::Http`.
			net::Endpoint Address;
			std::string Host;
		};

		// One file's bytes, hashed, shared by every destination it goes to.
		//
		// A `shared_ptr` rather than a copy per destination: two write origins
		// would otherwise mean two copies of a file in memory, and the whole
		// reason this uploads one file at a time is to hold exactly one.
		struct Payload {
			std::filesystem::path File;
			std::string Suffix;
			assets::ContentHash Root;
			std::vector<std::byte> Bytes;
		};

		// One file going to one destination, and where it has got to.
		struct Job {
			std::shared_ptr<const Payload> Content;
			size_t Destination = 0;

			// What this job is waiting on. `Probing` is the `HEAD` that asks
			// whether the origin already holds it; `Sending` is the `PUT`.
			enum class Stage : uint8_t { Queued, Probing, Sending } At = Stage::Queued;

			http::FetchId Fetch;
		};

		std::optional<std::vector<std::byte>> ReadWholeFile(const std::filesystem::path &path) {
			std::ifstream file(path, std::ios::binary | std::ios::ate);
			if (!file) {
				return std::nullopt;
			}
			const std::streamoff size = file.tellg();
			if (size < 0) {
				return std::nullopt;
			}
			file.seekg(0);

			std::vector<std::byte> bytes(static_cast<size_t>(size));
			if (size > 0) {
				file.read(reinterpret_cast<char *>(bytes.data()), size);
				if (!file) {
					return std::nullopt;
				}
			}
			return bytes;
		}

		// The extension, if it is one an origin will accept.
		//
		// **Lowercased and then checked, not repaired.** `cdn::Service` takes a
		// dot and up to fifteen lowercase alphanumerics and refuses everything
		// else, so anything outside that is dropped here rather than sent to be
		// refused — a `.PNG` becomes `.png`, and a `.tar.gz` loses nothing
		// because `extension()` already returned only `.gz`.
		std::string AcceptableSuffix(const std::filesystem::path &file) {
			std::string suffix = file.extension().string();
			if (suffix.size() < 2 || suffix.size() > 16 || suffix.front() != '.') {
				return {};
			}
			for (char &value : suffix) {
				if (value >= 'A' && value <= 'Z') {
					value = static_cast<char>(value - 'A' + 'a');
				}
			}
			for (const char value : std::string_view(suffix).substr(1)) {
				const bool allowed = (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
				if (!allowed) {
					return {};
				}
			}
			return suffix;
		}

		class FileUploader final : public Uploader {
		  public:
			explicit FileUploader(std::vector<Destination> destinations) : Targets(std::move(destinations)) {
				http::ClientSettings transfer;
				// Two in flight: the `HEAD` of the next job may overlap the
				// `PUT` of this one. More would defeat the one-file-in-memory
				// bound this class exists to keep.
				transfer.MaximumOutstanding = 2;
				transfer.IdlePolls = IDLE_POLLS;

				// A response to an upload is a status and nothing else, so the
				// half-gigabyte default would only ever bound a body that is
				// not coming.
				transfer.Limits.BodyBytes = 64ull * 1024u;
				Transfer = http::MakeClient(transfer);

				Descriptors.reserve(Targets.size());
				for (const Destination &target : Targets) {
					Descriptors.push_back(target.Descriptor);
				}
			}

			bool Add(const std::filesystem::path &file) override {
				std::optional<std::vector<std::byte>> bytes = ReadWholeFile(file);
				if (!bytes) {
					Tally.Failed++;
					Finished.push_back(
						UploadOutcome{
							.File = file,
							.Destination = {},
							.Root = {},
							.Detail = "could not be read",
							.Delivered = false,
						}
					);
					return false;
				}
				if (bytes->empty()) {
					// An empty file has no chunks, so `cdn::Publish` skips it
					// and an origin refuses it. Rejected here rather than sent
					// to be refused, so the reason names the file.
					Tally.Failed++;
					Finished.push_back(
						UploadOutcome{
							.File = file,
							.Destination = {},
							.Root = {},
							.Detail = "empty",
							.Delivered = false,
						}
					);
					return false;
				}

				auto payload = std::make_shared<Payload>();
				payload->Root = assets::Hasher::Of(*bytes);
				payload->File = file;
				payload->Suffix = AcceptableSuffix(file);
				payload->Bytes = std::move(*bytes);

				for (size_t index = 0; index < Targets.size(); index++) {
					Pending.push_back(
						Job{.Content = payload, .Destination = index, .At = Job::Stage::Queued, .Fetch = {}}
					);
				}
				return true;
			}

			size_t Pump() override {
				size_t finished = 0;

				// A directory destination completes inside this call, so the
				// loop runs until nothing more can be started or collected —
				// otherwise a hundred files to a local store would take a
				// hundred frames to copy.
				while (Start()) {
					// Started as much as the transfer client will hold.
				}

				if (!InFlight.empty()) {
					Transfer->Pump();
				}

				for (size_t index = 0; index < InFlight.size();) {
					if (Collect(InFlight[index])) {
						InFlight.erase(InFlight.begin() + static_cast<ptrdiff_t>(index));
						finished++;
						continue;
					}
					index++;
				}

				// Collecting frees a slot, so anything queued behind it starts
				// in the same call rather than waiting for the next.
				while (Start()) {}

				finished += Immediate;
				Immediate = 0;
				return finished;
			}

			size_t Remaining() const override {
				return Pending.size() + InFlight.size();
			}

			std::vector<UploadOutcome> Take() override {
				return std::exchange(Finished, {});
			}

			const UploadCounters &Counters() const override {
				return Tally;
			}

			const std::vector<Source> &Destinations() const override {
				return Descriptors;
			}

		  private:
			// How many polls an upload may go without progress. Generous
			// compared to a fetch's, because the thing on the far end is
			// writing a file to a disk rather than reading one it has already
			// prepared.
			static constexpr uint32_t IDLE_POLLS = 20000;

			// Starts one queued job if there is room.
			//
			// @return Whether anything started, so a caller can loop.
			bool Start() {
				// **One job in flight, and the probe is why.** Overlapping two
				// jobs lets both `HEAD`s go out before either `PUT` lands, so
				// two files with identical bytes each probe "not here" and each
				// send a body — the origin dedupes them correctly and the
				// uploader reports two stores, which is a lie about what
				// crossed the wire.
				//
				// The cost is one round trip of latency a file. The alternative
				// is tracking which hashes are in flight to which destination
				// and making later jobs wait on earlier ones, which is the same
				// serialisation with a data structure in front of it.
				if (Pending.empty() || !InFlight.empty()) {
					return false;
				}

				Job job = Pending.front();
				Pending.erase(Pending.begin());

				const Destination &target = Targets[job.Destination];
				if (target.Descriptor.Kind == SourceKind::Directory) {
					// **No round trip at all**: writing to a directory source
					// is writing to a filesystem this process already has, so
					// the probe and the store are one `exists` and one write.
					CopyLocally(job);
					Immediate++;
					return true;
				}

				job.At = Job::Stage::Probing;
				job.Fetch = Submit(job, http::Method::Head);
				if (!job.Fetch.IsValid()) {
					Fail(job, "could not be submitted");
					Immediate++;
					return true;
				}

				InFlight.push_back(std::move(job));
				return true;
			}

			http::FetchId Submit(const Job &job, http::Method verb) {
				const Destination &target = Targets[job.Destination];

				http::Request request;
				request.Verb = verb;

				// **The target is the hash re-rendered from the parsed value**,
				// which is also what the origin builds its filename from — so
				// the name a file lands under is decided by its bytes at both
				// ends and by nothing either side typed.
				request.Target = "/ingest/" + job.Content->Root.ToHex();
				request.Headers.push_back(
					http::Header{.Name = "x-atomic-ingest", .Value = target.Descriptor.IngestKey}
				);
				if (!job.Content->Suffix.empty()) {
					// The extension decides `assets::KindOfName` when the
					// origin publishes, so it is content and not decoration.
					request.Headers.push_back(
						http::Header{.Name = "x-atomic-suffix", .Value = job.Content->Suffix}
					);
				}
				if (verb == http::Method::Put) {
					request.Body = job.Content->Bytes;
				}

				return Transfer->Submit(target.Address, request, target.Host);
			}

			// @return Whether this job is finished and should be dropped.
			bool Collect(Job &job) {
				const http::FetchState state = Transfer->StateOf(job.Fetch);
				if (state == http::FetchState::Pending) {
					return false;
				}

				std::optional<http::Response> response = Transfer->Take(job.Fetch);
				if (!response) {
					Fail(job, "no answer");
					return true;
				}

				if (job.At == Job::Stage::Probing) {
					if (response->Code == http::Status::Ok) {
						// Already there. The bytes are the identity, so this is
						// a success that cost one round trip instead of a file.
						Tally.Skipped++;
						Report(job, "already there", true);
						return true;
					}
					if (response->Code == http::Status::Forbidden ||
						response->Code == http::Status::NotFound) {
						// **`404` here is ambiguous on purpose, at the far
						// end.** An origin that does not accept writes answers
						// the ingest route as though it were not there, so a
						// missing file and a read-only origin look the same —
						// see `cdn::Service::IngestOf`. The `PUT` resolves it:
						// a genuine miss stores, a read-only origin refuses
						// again and is reported as refused.
						job.At = Job::Stage::Sending;
						job.Fetch = Submit(job, http::Method::Put);
						if (!job.Fetch.IsValid()) {
							Fail(job, "could not be submitted");
							return true;
						}
						return false;
					}
					Fail(job, "probe answered " + std::to_string(static_cast<uint16_t>(response->Code)));
					return true;
				}

				if (response->Code == http::Status::Ok) {
					Tally.Stored++;
					Tally.SentBytes += job.Content->Bytes.size();
					Report(job, "stored", true);
					return true;
				}

				if (response->Code == http::Status::Forbidden || response->Code == http::Status::NotFound) {
					Tally.Refused++;
					Report(job, "refused — this origin does not accept this key", false);
					return true;
				}

				if (response->Code == http::Status::ContentTooLarge) {
					// Named rather than folded into "refused", because it is
					// the one refusal with an action attached: raise
					// `IngestSettings::MaximumFileBytes` at the origin.
					Tally.Refused++;
					Report(job, "larger than this origin accepts", false);
					return true;
				}

				Fail(job, "answered " + std::to_string(static_cast<uint16_t>(response->Code)));
				return true;
			}

			void CopyLocally(const Job &job) {
				const Destination &target = Targets[job.Destination];
				const std::filesystem::path directory(target.Descriptor.Location);

				std::error_code failure;
				std::filesystem::create_directories(directory, failure);
				if (failure) {
					Fail(job, "could not create " + directory.string());
					return;
				}

				const std::filesystem::path stored =
					directory / (job.Content->Root.ToHex() + job.Content->Suffix);
				if (std::filesystem::exists(stored, failure)) {
					Tally.Skipped++;
					Report(job, "already there", true);
					return;
				}

				std::ofstream out(stored, std::ios::binary | std::ios::trunc);
				if (!out) {
					Fail(job, "could not write " + stored.string());
					return;
				}
				out.write(
					reinterpret_cast<const char *>(job.Content->Bytes.data()),
					static_cast<std::streamsize>(job.Content->Bytes.size())
				);
				if (!out.good()) {
					Fail(job, "could not write " + stored.string());
					return;
				}

				Tally.Stored++;
				Tally.SentBytes += job.Content->Bytes.size();
				Report(job, "stored", true);
			}

			void Fail(const Job &job, std::string detail) {
				Tally.Failed++;
				Report(job, std::move(detail), false);
			}

			void Report(const Job &job, std::string detail, bool delivered) {
				Finished.push_back(
					UploadOutcome{
						.File = job.Content->File,
						.Destination = Targets[job.Destination].Descriptor.Name,
						.Root = job.Content->Root,
						.Detail = std::move(detail),
						.Delivered = delivered,
					}
				);
			}

			std::vector<Destination> Targets;
			std::vector<Source> Descriptors;
			std::unique_ptr<http::Client> Transfer;

			std::vector<Job> Pending;
			std::vector<Job> InFlight;
			std::vector<UploadOutcome> Finished;

			// Jobs that completed without ever being in flight — a directory
			// copy, or a submit that was refused a slot. Counted into the
			// return of the `Pump` that produced them.
			size_t Immediate = 0;

			UploadCounters Tally;
		};
	}

	std::unique_ptr<Uploader> MakeUploader(const DeliverySettings &settings) {
		std::vector<Destination> destinations;

		for (const Source &source : settings.Writable()) {
			Destination target;
			target.Descriptor = source;

			if (source.Kind == SourceKind::Http) {
				const std::optional<net::Endpoint> address = net::Endpoint::Parse(source.Location);
				if (!address) {
					// `Endpoint::Parse` refuses a host name on purpose, and the
					// reason is `AssetClient`'s unchanged: resolving one blocks
					// on a network service.
					ENGINE_WARN(
						"delivery: write source '{}' is not an address — {} (a host name needs resolving "
						"before it gets here)",
						source.Name,
						source.Location
					);
					continue;
				}
				target.Address = *address;
				target.Host = source.Location;
			}

			destinations.push_back(std::move(target));
		}

		if (destinations.empty()) {
			// **Absent rather than an object that does nothing.** A
			// configuration that has never been told where its write origin is
			// should not produce an uploader that accepts files and drops them.
			return nullptr;
		}

		ENGINE_INFO("delivery: {} write destination(s)", destinations.size());
		return std::make_unique<FileUploader>(std::move(destinations));
	}
}
