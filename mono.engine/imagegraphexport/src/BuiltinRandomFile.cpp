#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraph/BuiltinRandomCaptureCodec.hpp>
#include <engine/imagegraphexport/BuiltinRandomFile.hpp>
#include <engine/imagegraphexport/GraphFileHost.hpp>
#include <engine/imagegraphexport/GraphInputs.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>

namespace engine::imagegraphexport {
	using namespace engine::imagegraph;
	namespace {
		bool ReadText(
			const std::filesystem::path &path,
			const engine::assets::ContentPolicy &policy,
			uint64_t maximumBytes,
			std::string &text,
			std::string &failure
		) {
			ENGINE_PROFILE("imagegraph.builtin_random.read");
			if (!policy.AllowsName(path.string())) {
				failure = "builtin random input denied by content policy";
				return false;
			}
			std::error_code error;
			if (!std::filesystem::is_regular_file(path, error) || error) {
				failure = "builtin random input must be a regular file";
				return false;
			}
			const auto size = std::filesystem::file_size(path, error);
			if (error || size > maximumBytes) {
				failure = "builtin random input exceeds byte budget";
				return false;
			}
			std::string candidate(size_t(size), '\0');
			std::ifstream stream(path, std::ios::binary);
			stream.read(candidate.data(), std::streamsize(candidate.size()));
			if (!stream || size_t(stream.gcount()) != candidate.size() ||
				stream.peek() != std::char_traits<char>::eof()) {
				failure = "cannot read complete immutable builtin random input";
				return false;
			}
			engine::core::Metrics::Count("imagegraph.builtin_random.read_bytes", double(size));
			engine::core::Metrics::Count("imagegraph.builtin_random.read_operations", 1);
			text = std::move(candidate);
			return true;
		}
		bool SamePath(const std::filesystem::path &a, const std::filesystem::path &b) {
			std::error_code first, second;
			const auto left = std::filesystem::weakly_canonical(a, first),
					   right = std::filesystem::weakly_canonical(b, second);
			return first || second || left == right;
		}
	} // namespace
	bool ParseBuiltinRandomDraw(std::string_view text, SourceBuiltinRandomDraw &draw, std::string &failure) {
		if (text.size() > 256) {
			failure = "builtin random draw must be bounded operation:lower:upper:result";
			return false;
		}
		const auto separator = text.find(':');
		const auto operation = ParseBuiltinRandomOperationName(text.substr(0, separator));
		if (!operation || separator == std::string_view::npos) {
			failure = "unknown builtin random operation name";
			return false;
		}
		SourceBuiltinRandomDraw candidate;
		candidate.Operation = *operation;
		text.remove_prefix(separator + 1);
		for (double *field : {&candidate.Lower, &candidate.Upper, &candidate.Result}) {
			const size_t end = text.find(':');
			const auto number = text.substr(0, end);
			const auto parsed = std::from_chars(number.data(), number.data() + number.size(), *field);
			if (number.empty() || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() ||
				!std::isfinite(*field)) {
				failure = "builtin random draw needs three finite literal numbers";
				return false;
			}
			if ((field == &candidate.Result) != (end == std::string_view::npos)) {
				failure = "builtin random draw needs exactly three numbers";
				return false;
			}
			if (end != std::string_view::npos) text.remove_prefix(end + 1);
		}
		draw = candidate;
		return true;
	}
	bool LoadBuiltinRandomCaptureFile(
		const std::filesystem::path &file,
		const engine::assets::ContentPolicy &policy,
		std::vector<SourceBuiltinRandomCapture> &captures,
		std::string &failure,
		uint64_t maximumBytes
	) {
		ENGINE_PROFILE("imagegraph.builtin_random.load");
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		uint64_t previousBytes = 0;
		Diagnostic diagnostic;
		if (ValidateBuiltinRandomCaptures(captures, maximumBytes, previousBytes, diagnostic) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		if (captures.capacity() > (maximumBytes - previousBytes) / sizeof(SourceBuiltinRandomCapture)) {
			failure = "builtin random previous capture slots exceed residency";
			return false;
		}
		const uint64_t slots = captures.capacity() * sizeof(SourceBuiltinRandomCapture);
		std::string text;
		if (!ReadText(file, policy, maximumBytes - previousBytes - slots, text, failure)) return false;
		if (ReadBuiltinRandomCapture(text, captures, diagnostic, maximumBytes) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		return true;
	}
	bool PrepareBuiltinRandomCaptureFile(
		const GraphExportSettings &settings,
		std::string_view nodeId,
		const std::filesystem::path &destination,
		std::span<const SourceBuiltinRandomDraw> draws,
		std::string &failure,
		uint64_t maximumBytes
	) {
		ENGINE_PROFILE("imagegraph.builtin_random.prepare_file");
		maximumBytes = std::min(maximumBytes, Limits::MaximumEvaluationBytes);
		if (nodeId.empty() || draws.empty() || draws.size() > 65536 ||
			SamePath(settings.Input, destination)) {
			failure = "builtin random preparation needs a node, 1..65536 observed "
					  "draws and a distinct destination";
			return false;
		}
		for (const auto &image : settings.ImageInputs) {
			if (SamePath(image.File, destination)) {
				failure = "builtin random destination must differ from image inputs";
				return false;
			}
		}
		std::string graphText;
		if (!ReadText(
				settings.Input,
				settings.Content,
				std::min<uint64_t>(maximumBytes, Limits::MaximumDocumentBytes),
				graphText,
				failure
			))
			return false;
		Document document;
		Plan plan;
		Diagnostic diagnostic;
		if (Read(graphText, document, diagnostic) != Status::Ok ||
			Compile(document, plan, diagnostic) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		std::vector<RequestImageSource> sources;
		if (!LoadGraphImageInputs(settings, sources, failure, maximumBytes)) return false;
		EvaluationRequest request;
		request.Tick = settings.Frames.First;
		request.RigidPlaying = settings.RigidPlaying;
		request.RigidFrameProgress = settings.RigidFrameProgress;
		request.ImageSources = sources;
		request.HostCaptures = settings.HostCaptures;
		request.HostProvider = settings.HostProvider;
		SourceBuiltinRandomCapture captured;
		if (PrepareSourceBuiltinRandomCapture(
				document, plan, nodeId, request, captured, diagnostic, maximumBytes
			) != Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		uint64_t captureBytes = 0;
		if (ValidateBuiltinRandomCaptures(std::span(&captured, 1), maximumBytes, captureBytes, diagnostic) !=
				Status::Ok ||
			draws.size() * sizeof(SourceBuiltinRandomDraw) > maximumBytes - captureBytes) {
			failure = "builtin random observed draws exceed capture budget";
			return false;
		}
		captured.Draws.assign(draws.begin(), draws.end());
		std::string text;
		if (WriteBuiltinRandomCapture(std::span(&captured, 1), text, diagnostic, maximumBytes) !=
			Status::Ok) {
			failure = diagnostic.Message;
			return false;
		}
		const GraphFileGrant grant{std::string(nodeId), destination, true};
		return PublishGraphHostFile(
			grant, settings.Content, std::as_bytes(std::span(text.data(), text.size())), maximumBytes, failure
		);
	}
} // namespace engine::imagegraphexport
