#include <engine/assets/ContentHash.hpp>
#include <engine/script/DataScriptExecutor.hpp>

#include <span>

namespace engine::script {
	DataScriptResult ExecuteDataScript(
		world::Universe &universe,
		world::DataFactorySession &session,
		const DataScriptRequest &request,
		const DataScriptRuntimeResolver &runtimeOf
	) {
		DataScriptResult result;
		if (request.Source.size() > 256u * 1024u) {
			result.Error = "source exceeds 256 KiB";
			return result;
		}
		const auto expected = assets::ContentHash::FromHex(request.SourceHash);
		const auto actual =
			assets::Hasher::Of(std::as_bytes(std::span(request.Source.data(), request.Source.size())));
		if (!expected || *expected != actual) {
			result.Error = "source_hash does not match blake3-256 source";
			return result;
		}
		(void)universe;
		(void)session;
		(void)runtimeOf;
		result.Error = "data-script execution is not installed";
		return result;
	}
}
