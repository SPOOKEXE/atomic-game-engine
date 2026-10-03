#include <engine/assets/ContentPolicy.hpp>
#include <engine/core/Metrics.hpp>
#include <engine/core/Profiling.hpp>
#include <engine/render/CookedComposer.hpp>

#include <fstream>
#include <new>
namespace engine::render::hlsl {
	std::optional<std::string> ReadArtifact(
		const std::filesystem::path &directory,
		engine::core::Name name,
		engine::assets::ShaderData &output,
		uint64_t maximumBytes
	) try {
		ENGINE_PROFILE_CAT("composer local cooked artifact", engine::core::ProfileCategory::Assets);
		const auto text = name.Text();
		if (text.size() != 72 || !text.ends_with(".ashader") ||
			!engine::assets::ContentHash::FromHex(text.substr(0, 64)))
			return "Local Composer artifact must use its ASH1 content address";
		const auto &policy = engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle);
		if (!policy.AllowsName(text)) return "Composer artifact handling is disabled";
		const auto path = directory / std::string(text);
		std::error_code error;
		const auto size = std::filesystem::file_size(path, error);
		if (error || size == 0 || size > engine::assets::Shader::MAXIMUM_BYTES)
			return "Local Composer artifact is unavailable or exceeds limit";
		const auto priorBytes = engine::assets::ShaderRetainedPayloadBytes(output, maximumBytes);
		if (!priorBytes || size > maximumBytes - *priorBytes ||
			sizeof(engine::assets::ShaderData) > maximumBytes - *priorBytes - size)
			return "Composer artifact read exceeds operation residency budget";
		std::ifstream file(path, std::ios::binary);
		if (!file) return "Local Composer artifact cannot be opened";
		std::vector<std::byte> bytes(size);
		if (!file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size)) ||
			file.peek() != std::char_traits<char>::eof())
			return "Local Composer artifact changed during bounded read";
		if (bytes.capacity() > maximumBytes - *priorBytes)
			return "Composer artifact encoded capacity exceeds operation budget";
		engine::core::Metrics::Count("composer.local_artifact_read_bytes", size);
		engine::core::Metrics::Count("composer.local_artifact_reads", 1);
		if (engine::assets::Hasher::Of(bytes).ToHex() != text.substr(0, 64))
			return "Local Composer artifact content address mismatches bytes";
		engine::core::ByteReader reader(bytes);
		engine::assets::ShaderData candidate;
		if (!engine::assets::Shader::Read(reader, candidate, maximumBytes - *priorBytes - bytes.capacity()))
			return "Local Composer artifact container is invalid";
		output = std::move(candidate);
		return {};
	} catch (const std::bad_alloc &) {
		return "Local Composer artifact allocation failed";
	}
} // namespace engine::render::hlsl
