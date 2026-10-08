#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/imagegraphexport/GraphImageCache.hpp>

#include <fstream>
#include <new>

namespace engine::imagegraphexport {
	bool CookGraphSourceFrameCaches(
		const std::filesystem::path &input,
		const std::filesystem::path &output,
		std::span<const GraphImageCacheLayoutObservation> observations,
		const assets::ContentPolicy &policy,
		std::string &failure,
		uint64_t maximumBytes
	) try {
		using namespace imagegraph;
		ENGINE_PROFILE("imagegraphexport.frame_cache_cook");
		const auto fail = [&](const char *message) {
			failure = message;
			return false;
		};
		if (maximumBytes > Limits::MaximumEvaluationBytes || maximumBytes < 4096 || observations.empty() ||
			observations.size() > 64 || input.empty() || output.empty() || input.native().size() > 4096 ||
			output.native().size() > 4096 || output.extension() != ".graph" ||
			!policy.AllowsName(input.string()) || !policy.AllowsName(output.string()))
			return fail("frame-cache cooking requires bounded receipts and allowed native graph paths");
		std::error_code error;
		const auto absoluteInput = std::filesystem::weakly_canonical(input, error);
		if (error) return fail("cannot resolve frame-cache cook input");
		const auto absoluteOutput = std::filesystem::weakly_canonical(output, error);
		if (error || absoluteInput == absoluteOutput)
			return fail("frame-cache cooking requires a separate exact native graph output");
		if (std::filesystem::exists(output, error) && std::filesystem::equivalent(input, output, error))
			return fail("frame-cache cooking cannot replace its source through an alias");
		if (error) return fail("cannot inspect frame-cache cook destination");
		Document cooked;
		Diagnostic diagnostic;
		{
			const auto bytes = std::filesystem::file_size(input, error);
			if (error || bytes > Limits::MaximumDocumentBytes || bytes > maximumBytes / 16)
				return fail("frame-cache cook source exceeds its staged input budget");
			std::string text(static_cast<size_t>(bytes), '\0');
			std::ifstream stream(input, std::ios::binary);
			stream.read(text.data(), static_cast<std::streamsize>(text.size()));
			if (!stream || static_cast<size_t>(stream.gcount()) != text.size() || stream.peek() != EOF)
				return fail("cannot read one complete frame-cache cook source");
			Document source;
			core::Metrics::Count("imagegraphexport.frame_cache_cook_read_bytes", double(bytes));
			if (Read(text, source, diagnostic, maximumBytes / 4) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			const auto sourceBytes = DocumentRetainedPayloadBytes(source);
			if (!sourceBytes || *sourceBytes > maximumBytes - text.capacity())
				return fail("frame-cache source generations exceed compilation budget");
			{
				Plan plan;
				if (Compile(source, plan, diagnostic, maximumBytes - text.capacity() - *sourceBytes) !=
					Status::Ok) {
					failure = diagnostic.Message;
					return false;
				}
			}

			std::string{}.swap(text);
			if (imagegraphio::CookSourceFrameCaches(
					source, observations, cooked, diagnostic, maximumBytes / 2
				) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
		}
		const auto retained = DocumentRetainedPayloadBytes(cooked);
		// Bound quoted text expansion and overlapping serializer buffers before writing any text.
		if (!retained || *retained > (maximumBytes - 1024) / 8 ||
			*retained > (Limits::MaximumDocumentBytes - 1024) / 4)
			return fail("frame-cache cook text expansion exceeds its byte budget");
		const auto text = Write(cooked);
		if (text.empty() || text.size() > Limits::MaximumDocumentBytes ||
			text.capacity() > maximumBytes - *retained)
			return fail("frame-cache cook encoded document exceeds byte bounds");
		{
			Document checked;
			Plan plan;
			if (Read(text, checked, diagnostic, maximumBytes - *retained - text.capacity()) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			const auto checkedBytes = DocumentRetainedPayloadBytes(checked);
			const uint64_t remaining = maximumBytes - *retained - text.capacity();
			if (!checkedBytes || *checkedBytes > remaining)
				return fail("frame-cache checked document exceeds compilation budget");
			if (Compile(checked, plan, diagnostic, remaining - *checkedBytes) != Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			if (checked != cooked) return fail("frame-cache cook document did not roundtrip exactly");
		}
		const GraphFileGrant grant{"frame_cache_cook", output, true};
		if (!PublishGraphHostFile(
				grant,
				policy,
				std::as_bytes(std::span(text.data(), text.size())),
				Limits::MaximumDocumentBytes,
				failure
			))
			return false;
		core::Metrics::Count("imagegraphexport.frame_cache_cook_write_bytes", double(text.size()));
		failure.clear();
		return true;
	} catch (const std::bad_alloc &) {
		failure = "frame-cache cook allocation failed";
		return false;
	} catch (const std::filesystem::filesystem_error &) {
		failure = "frame-cache cook filesystem operation failed";
		return false;
	}
}
