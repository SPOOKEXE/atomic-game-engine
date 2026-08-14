// Asking one origin what it holds, and reading what it says back.
//
// **Split from `AssetCatalogue.cpp` because only half of this can be checked
// without a port.** Deciding what a tab says is arithmetic over an outcome; the
// bytes on the wire need something listening. Keeping them in one file would
// have made the first half as expensive to test as the second, and note text is
// the whole of what this feature delivers to somebody looking at the panel.
//
// **It blocks, and the ceiling is the design.** `studio/AssetCatalogue.hpp`
// carries the argument at `MakeOriginLister`: a listing is asked for when
// somebody opens or refreshes the panel, so waiting for the answer with a bound
// on the wait is cheaper - in code and in what can go wrong - than the per-tab
// delivery client with its own lifetime and its own pump that D00111 rejected.

#include <engine/assets/AssetKind.hpp>
#include <engine/assets/ContentHash.hpp>
#include <engine/core/Log.hpp>
#include <engine/net/Endpoint.hpp>
#include <engine/net/http/Client.hpp>

#include <charconv>
#include <chrono>
#include <string>
#include <studio/AssetCatalogue.hpp>
#include <thread>
#include <utility>

namespace studio {

	namespace {
		namespace http = engine::net::http;
		using engine::delivery::Source;
		using engine::delivery::SourceKind;
		using engine::net::Endpoint;

		// The format's first line, which is also its version.
		//
		// A page that says anything else is refused rather than guessed at: a
		// reader that carries on into a layout it does not know is a reader
		// mis-parsing bytes an origin chose.
		constexpr std::string_view PAGE_HEADER = "catalogue 1";

		// One line, without the newline and without a stray carriage return.
		std::string_view LineAt(std::string_view body, size_t &at) {
			const size_t end = body.find('\n', at);
			std::string_view line =
				body.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
			at = end == std::string_view::npos ? body.size() : end + 1;
			if (!line.empty() && line.back() == '\r') {
				line.remove_suffix(1);
			}
			return line;
		}

		std::optional<uint64_t> Number(std::string_view text) {
			uint64_t value = 0;
			const char *const end = text.data() + text.size();
			const std::from_chars_result read = std::from_chars(text.data(), end, value);
			if (read.ec != std::errc{} || read.ptr != end) {
				return std::nullopt;
			}
			return value;
		}

		// `asset <root> <kind> <bytes> <name>`.
		//
		// The name is the rest of the line, which is why it is written last: it
		// is the one field with no bound on what is inside it, and a separator
		// after it would be one a name could contain.
		std::optional<CatalogueEntry> ParseAsset(std::string_view line, std::string_view source) {
			const auto next = [&line]() -> std::optional<std::string_view> {
				const size_t space = line.find(' ');
				if (space == std::string_view::npos) {
					return std::nullopt;
				}
				const std::string_view field = line.substr(0, space);
				line.remove_prefix(space + 1);
				return field;
			};

			const std::optional<std::string_view> root = next();
			const std::optional<std::string_view> kind = next();
			const std::optional<std::string_view> bytes = next();
			if (!root || !kind || !bytes || line.empty()) {
				return std::nullopt;
			}

			const std::optional<engine::assets::ContentHash> address =
				engine::assets::ContentHash::FromHex(*root);
			if (!address || !Number(*bytes)) {
				return std::nullopt;
			}

			return CatalogueEntry{
				.Name = std::string(line),
				// An unknown kind reads as `Unknown` rather than refusing the
				// row: a newer origin publishing a kind this editor has not
				// heard of is still content it holds, and hiding the name would
				// be the panel lying about the origin again.
				.Kind = engine::assets::KindFromName(*kind),
				.Root = *address,
				.Source = std::string(source),
				.Unbaked = {},
			};
		}

		// The fetching lister: one client, one origin, pages until the cursor
		// runs out.
		class HttpOriginLister final : public OriginLister {
		  public:
			explicit HttpOriginLister(const OriginListerSettings &settings) : Bounds(settings) {}

			OriginListing List(const Source &source) override {
				OriginListing listing;

				if (source.Kind != SourceKind::Http) {
					// A directory is listed by reading its manifest and never
					// by asking it anything - `DirectoryAssets`.
					listing.Outcome = ListingOutcome::NotAsked;
					return listing;
				}
				if (source.IngestKey.empty()) {
					// **Not even a round trip.** An origin does not enumerate
					// for an unauthenticated caller, so asking would spend a
					// connection to be told what is already known here.
					listing.Outcome = ListingOutcome::NoKey;
					return listing;
				}

				const std::optional<Endpoint> address = Endpoint::Parse(source.Location);
				if (!address) {
					// `Endpoint::Parse` refuses a host name on purpose, which
					// is the delivery client's limitation too - an address this
					// editor cannot reach is the same outcome as one nobody
					// answers at.
					listing.Outcome = ListingOutcome::Unreachable;
					return listing;
				}

				http::ClientSettings transfer;
				transfer.IdlePolls = Bounds.IdlePolls;
				const std::unique_ptr<http::Client> fetcher = http::MakeClient(transfer);

				std::string target = "/catalogue";
				uint64_t walked = 0;

				for (;;) {
					const std::optional<http::Response> answer = Ask(*fetcher, *address, source, target);
					if (!answer) {
						return Only(ListingOutcome::Unreachable);
					}

					switch (answer->Code) {
					case http::Status::Ok:
						break;
					case http::Status::NotFound:
						return Only(ListingOutcome::NotOffered);
					case http::Status::Forbidden:
						return Only(ListingOutcome::Refused);
					case http::Status::ServiceUnavailable:
						// Up, and nothing published. A true empty listing
						// rather than a failure - the empty case has its own
						// sentence and it is the right one.
						return Only(ListingOutcome::Listed);
					default:
						ENGINE_WARN(
							"catalogue: {} answered {} for {}",
							source.Location,
							static_cast<uint16_t>(answer->Code),
							target
						);
						return Only(ListingOutcome::Unreadable);
					}

					const std::string_view body(
						reinterpret_cast<const char *>(answer->Body.data()), answer->Body.size()
					);
					const std::optional<CataloguePage> page = ParseCataloguePage(body, source.Name);
					if (!page) {
						ENGINE_WARN(
							"catalogue: {} answered something that is not a listing", source.Location
						);
						return Only(ListingOutcome::Unreadable);
					}

					for (const CatalogueEntry &entry : page->Entries) {
						if (listing.Entries.size() >= Bounds.MaximumEntries) {
							ENGINE_WARN(
								"catalogue: {} listed more than {} assets - the rest is not shown",
								source.Location,
								Bounds.MaximumEntries
							);
							listing.Outcome = ListingOutcome::Listed;
							return listing;
						}
						listing.Entries.push_back(entry);
					}

					if (!page->Next) {
						break;
					}
					if (*page->Next <= walked) {
						// A cursor that does not move is a walk that never
						// ends. Refused rather than followed: an origin is
						// something anybody can run.
						ENGINE_WARN(
							"catalogue: {} handed back a cursor that does not advance", source.Location
						);
						return Only(ListingOutcome::Unreadable);
					}

					walked = *page->Next;
					target = "/catalogue/" + std::to_string(walked);
				}

				listing.Outcome = ListingOutcome::Listed;
				return listing;
			}

		  private:
			// An outcome with nothing behind it.
			//
			// **Entries are dropped with the failure**, because a half-collected
			// listing under a "could not list" sentence is exactly the confident
			// wrongness this feature exists to avoid.
			static OriginListing Only(ListingOutcome outcome) {
				return OriginListing{.Outcome = outcome, .Entries = {}};
			}

			// One request, waited for with a ceiling on the wait.
			std::optional<http::Response>
			Ask(http::Client &fetcher,
				const Endpoint &address,
				const Source &source,
				const std::string &target) {
				http::Request request;
				request.Verb = http::Method::Get;
				request.Target = target;
				// **The same header an upload sends**, because it is the same
				// secret: `cdn::CatalogueSettings` reuses the ingest key rather
				// than growing a second way to be admitted.
				request.Headers.push_back(http::Header{.Name = "x-atomic-ingest", .Value = source.IngestKey});

				const http::FetchId id = fetcher.Submit(address, request, source.Location);
				if (!id.IsValid()) {
					return std::nullopt;
				}

				for (uint32_t poll = 0; poll < Bounds.MaximumPolls; poll++) {
					fetcher.Pump();
					if (fetcher.StateOf(id) != http::FetchState::Pending) {
						break;
					}
					std::this_thread::sleep_for(std::chrono::microseconds(Bounds.PollMicroseconds));
				}

				if (fetcher.StateOf(id) != http::FetchState::Ready) {
					fetcher.Cancel(id);
					return std::nullopt;
				}
				return fetcher.Take(id);
			}

			OriginListerSettings Bounds;
		};
	}

	std::optional<CataloguePage> ParseCataloguePage(std::string_view body, std::string_view source) {
		size_t at = 0;
		if (LineAt(body, at) != PAGE_HEADER) {
			return std::nullopt;
		}

		CataloguePage page;
		while (at < body.size()) {
			const std::string_view line = LineAt(body, at);
			if (line.empty()) {
				continue;
			}

			const size_t space = line.find(' ');
			if (space == std::string_view::npos) {
				return std::nullopt;
			}
			const std::string_view key = line.substr(0, space);
			const std::string_view value = line.substr(space + 1);

			if (key == "asset") {
				std::optional<CatalogueEntry> entry = ParseAsset(value, source);
				if (!entry) {
					// A row that does not parse means the format is not the one
					// this reader knows, so the page is refused whole rather
					// than shown with a gap in it.
					return std::nullopt;
				}
				page.Entries.push_back(std::move(*entry));
				continue;
			}

			if (key == "root") {
				if (!engine::assets::ContentHash::FromHex(value)) {
					return std::nullopt;
				}
				page.Root = std::string(value);
				continue;
			}

			const std::optional<uint64_t> number = Number(value);
			if (key == "total") {
				if (!number) {
					return std::nullopt;
				}
				page.Total = *number;
				continue;
			}
			if (key == "next") {
				if (!number) {
					return std::nullopt;
				}
				page.Next = *number;
				continue;
			}

			// Anything else in a version-1 page is a line added by a newer
			// origin. Ignored rather than refused: the version guards the
			// *shape*, and refusing a page over a field nobody reads would make
			// every future addition a breaking one.
		}

		return page;
	}

	std::unique_ptr<OriginLister> MakeOriginLister(const OriginListerSettings &settings) {
		return std::make_unique<HttpOriginLister>(settings);
	}
}
