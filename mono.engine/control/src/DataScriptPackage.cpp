#include <engine/control/DataFactoryOperationLedger.hpp>
#include <engine/control/DataScriptPackage.hpp>
#include <engine/script/DataScriptPackage.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <nlohmann/json.hpp>
#include <utility>

namespace engine::control {
	using nlohmann::json;

	namespace {
		constexpr size_t MAXIMUM_ID = 128;

		std::string Error(std::string_view code, std::string_view detail) {
			return std::string(code) + ": " + std::string(detail);
		}

		bool Text(
			const json &value, std::string_view name, size_t maximum, std::string &out, std::string &failure
		) {
			if (!value.is_string() || value.get_ref<const std::string &>().empty() ||
				value.get_ref<const std::string &>().size() > maximum) {
				failure =
					Error("validation_failed", std::string(name) + " must be a bounded nonempty string");
				return false;
			}
			out = value.get<std::string>();
			return true;
		}

		bool UInt(const json &value, std::string_view name, uint64_t &out, std::string &failure) {
			if (value.is_number_unsigned()) {
				out = value.get<uint64_t>();
				return true;
			}
			if (!value.is_number_integer()) {
				failure = Error("validation_failed", std::string(name) + " must be an unsigned integer");
				return false;
			}
			if (value.get<int64_t>() < 0) {
				failure = Error("validation_failed", std::string(name) + " must be nonnegative");
				return false;
			}
			out = value.get<uint64_t>();
			return true;
		}

		bool DecodeBase64(
			const json &value,
			std::string_view name,
			size_t maximum,
			std::vector<std::byte> &out,
			std::string &failure
		) {
			const size_t maximumEncoded = (maximum / 3) * 4 + (maximum % 3 == 0 ? 0 : 4);
			if (!value.is_string() || value.get_ref<const std::string &>().size() > maximumEncoded) {
				failure = Error("validation_failed", std::string(name) + " must be bounded base64");
				return false;
			}
			const std::string &encoded = value.get_ref<const std::string &>();
			if (encoded.size() % 4 != 0) {
				failure = Error("validation_failed", std::string(name) + " is not canonical base64");
				return false;
			}
			const auto digit = [](char character) -> int {
				if (character >= 'A' && character <= 'Z') return character - 'A';
				if (character >= 'a' && character <= 'z') return character - 'a' + 26;
				if (character >= '0' && character <= '9') return character - '0' + 52;
				if (character == '+') return 62;
				if (character == '/') return 63;
				return -1;
			};
			out.clear();
			out.reserve(encoded.size() / 4 * 3);
			for (size_t at = 0; at < encoded.size(); at += 4) {
				const bool padded2 = encoded[at + 2] == '=';
				const bool padded3 = encoded[at + 3] == '=';
				if ((padded2 && !padded3) || ((padded2 || padded3) && at + 4 != encoded.size())) {
					failure = Error("validation_failed", std::string(name) + " is not canonical base64");
					return false;
				}
				const int a = digit(encoded[at]);
				const int b = digit(encoded[at + 1]);
				const int c = padded2 ? 0 : digit(encoded[at + 2]);
				const int d = padded3 ? 0 : digit(encoded[at + 3]);
				if (a < 0 || b < 0 || c < 0 || d < 0 || (padded2 && (b & 15) != 0) ||
					(padded3 && !padded2 && (c & 3) != 0)) {
					failure = Error("validation_failed", std::string(name) + " is not canonical base64");
					return false;
				}
				if (out.size() + 3 - static_cast<size_t>(padded2) - static_cast<size_t>(padded3) > maximum) {
					failure = Error("validation_failed", std::string(name) + " exceeds the byte limit");
					return false;
				}
				out.push_back(static_cast<std::byte>((a << 2) | (b >> 4)));
				if (!padded2) out.push_back(static_cast<std::byte>((b << 4) | (c >> 2)));
				if (!padded3) out.push_back(static_cast<std::byte>((c << 6) | d));
			}
			return true;
		}

		json Reply(const engine::script::DataScriptResult &result) {
			return {
				{"schema_version", "atomic.data-script.v1"},
				{"status", result.Ran ? "completed" : "failed"},
				{"terminal", result.Ran ? "completed" : "failed"},
				{"ran", result.Ran},
				{"atomic", result.Atomic},
				{"language", "luau"},
				{"error", result.Error},
				{"instance_id", result.Lifecycle.InstanceId},
				{"tick", result.Lifecycle.Clock.Tick},
				{"world_epoch", result.Lifecycle.WorldEpoch},
				{"world_version", result.Lifecycle.WorldVersion},
				{"source_hash", result.Package ? result.Package->SourceHash.ToHex() : ""},
			};
		}
	}

	void AddDataScriptPackageTool(Surface &surface, DataScriptPackageExecutor execute) {
		auto ledger = surface.DataFactoryOperations();
		surface.Add(
			{"run_script_package",
			 "Runs one atomic.data-script.v1 package in a fresh capability-limited Luau sandbox and "
			 "atomically replaces a paused factory world.",
			 [] {
				 json properties;
				 properties["instance_id"] = {
					 {"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}
				 };
				 properties["expected_tick"] = {{"type", "integer"}, {"minimum", 0}};
				 properties["expected_world_epoch"] = {{"type", "integer"}, {"minimum", 0}};
				 properties["expected_world_version"] = {{"type", "integer"}, {"minimum", 0}};
				 properties["operation_id"] = {
					 {"type", "string"}, {"minLength", 1}, {"maxLength", MAXIMUM_ID}
				 };
				 properties["language"] = {{"type", "string"}, {"enum", {"luau"}}};
				 properties["manifest"] = {
					 {"type", "string"},
					 {"minLength", 1},
					 {"maxLength", engine::script::DATA_SCRIPT_PACKAGE_MAX_MANIFEST_BYTES}
				 };
				 properties["source_base64"] = {
					 {"type", "string"},
					 {"maxLength", engine::script::DATA_SCRIPT_PACKAGE_MAX_SOURCE_BYTES * 2}
				 };
				 properties["assets"] = {
					 {"type", "array"},
					 {"maxItems", engine::script::DATA_SCRIPT_PACKAGE_MAX_ASSETS},
					 {"items",
					  {{"type", "object"},
					   {"additionalProperties", false},
					   {"properties",
						{{"path", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
						 {"base64",
						  {{"type", "string"},
						   {"maxLength", engine::script::DATA_SCRIPT_PACKAGE_MAX_ASSET_BYTES * 2}}}}},
					   {"required", {"path", "base64"}}}}
				 };
				 json schema;
				 schema["type"] = "object";
				 schema["additionalProperties"] = false;
				 schema["properties"] = std::move(properties);
				 schema["required"] = json::array(
					 {"instance_id",
					  "expected_tick",
					  "expected_world_epoch",
					  "expected_world_version",
					  "operation_id",
					  "language",
					  "manifest",
					  "source_base64",
					  "assets"}
				 );
				 return schema;
			 },
			 [execute = std::move(execute), ledger](const json &values, std::string &failure) -> json {
				 static constexpr std::array<std::string_view, 9> fields{
					 "instance_id",
					 "expected_tick",
					 "expected_world_epoch",
					 "expected_world_version",
					 "operation_id",
					 "language",
					 "manifest",
					 "source_base64",
					 "assets"
				 };
				 if (!values.is_object() || values.size() != fields.size()) {
					 failure = Error(
						 "validation_failed", "arguments must use the atomic.data-script.v1 wire schema"
					 );
					 return nullptr;
				 }
				 for (const auto field : fields)
					 if (!values.contains(field)) {
						 failure = Error(
							 "validation_failed", "arguments must use the atomic.data-script.v1 wire schema"
						 );
						 return nullptr;
					 }
				 engine::script::DataScriptRequest request;
				 if (!Text(values["instance_id"], "instance_id", MAXIMUM_ID, request.InstanceId, failure) ||
					 !Text(values["operation_id"], "operation_id", MAXIMUM_ID, request.Name, failure) ||
					 !Text(
						 values["manifest"],
						 "manifest",
						 engine::script::DATA_SCRIPT_PACKAGE_MAX_MANIFEST_BYTES,
						 request.Manifest,
						 failure
					 ) ||
					 !UInt(values["expected_tick"], "expected_tick", request.ExpectedTick, failure) ||
					 !UInt(
						 values["expected_world_epoch"],
						 "expected_world_epoch",
						 request.ExpectedEpoch,
						 failure
					 ) ||
					 !UInt(
						 values["expected_world_version"],
						 "expected_world_version",
						 request.ExpectedVersion,
						 failure
					 ) ||
					 !values["language"].is_string() || values["language"] != "luau") {
					 if (failure.empty())
						 failure = Error("capability_unsupported", "only Luau packages are available");
					 return nullptr;
				 }
				 const auto manifest = engine::script::ParseDataScriptPackage(request.Manifest);
				 if (!manifest) {
					 failure = Error("validation_failed", manifest.Error);
					 return nullptr;
				 }
				 const size_t sourceLimit = static_cast<size_t>(manifest.Package->Budget.SourceBytes);
				 const size_t assetLimit = static_cast<size_t>(manifest.Package->Budget.AssetBytes);
				 std::vector<std::byte> source;
				 if (!DecodeBase64(values["source_base64"], "source_base64", sourceLimit, source, failure))
					 return nullptr;
				 if (source.empty())
					 request.Source.clear();
				 else
					 request.Source.assign(reinterpret_cast<const char *>(source.data()), source.size());
				 const std::string operation = request.Name;
				 if (!values["assets"].is_array() ||
					 values["assets"].size() > engine::script::DATA_SCRIPT_PACKAGE_MAX_ASSETS) {
					 failure = Error("validation_failed", "assets must contain at most 128 rows");
					 return nullptr;
				 }
				 size_t aggregate = 0;
				 for (const json &asset : values["assets"]) {
					 engine::script::DataScriptAssetInput input;
					 if (!asset.is_object() || asset.size() != 2 || !asset.contains("path") ||
						 !asset.contains("base64")) {
						 failure = Error("validation_failed", "each asset must contain only path and base64");
						 return nullptr;
					 }
					 if (!Text(asset["path"], "asset path", 256, input.Path, failure) ||
						 !DecodeBase64(
							 asset["base64"], "asset base64", assetLimit - aggregate, input.Bytes, failure
						 ))
						 return nullptr;
					 aggregate += input.Bytes.size();
					 request.Assets.push_back(std::move(input));
				 }
				 const std::string serialized = values.dump();
				 json replay;
				 const auto prior =
					 ledger->Replay("run_script_package", operation, serialized, replay, failure);
				 if (prior == engine::control::DataFactoryOperationReplay::Conflict) return nullptr;
				 if (prior == engine::control::DataFactoryOperationReplay::Replay) return replay;
				 engine::script::DataScriptResult result;
				 try {
					 result = execute(request);
				 } catch (const std::exception &error) {
					 result.Error = "package execution failed: " + std::string(error.what());
				 } catch (...) {
					 result.Error = "package execution failed";
				 }
				 if (result.Lifecycle.InstanceId.empty()) result.Lifecycle.InstanceId = request.InstanceId;
				 if (!result.Package) result.Package = *manifest.Package;
				 if (!result.Ran && result.Error.empty()) result.Error = "package execution failed";
				 json reply = Reply(result);
				 failure.clear();
				 ledger->Store("run_script_package", operation, serialized, reply, failure);
				 return reply;
			 }}
		);
	}
}
