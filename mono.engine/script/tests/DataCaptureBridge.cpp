#include <engine/ecs/Store.hpp>
#include <engine/script/DataCaptureBridge.hpp>
#include <engine/script/DataSceneService.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string_view>

TEST_SUITE_ID("engine.script.datacapturebridge")

namespace {
	const engine::script::ScriptValue *Field(const engine::script::ScriptValue &map, std::string_view name) {
		for (const auto &[key, value] : map.Entries)
			if (key == name) return &value;
		return nullptr;
	}

	class CaptureBridge final : public engine::script::DataCaptureBridge {
	  public:
		engine::script::DataCaptureBridgeCapabilities Capabilities() const override {
			return {
				.Available = true,
				.Channels = {"rgb_linear_hdr"},
				.StorageProfiles = {"lossless", "training_compact"},
				.TrainingCompactLimitations = {"linear_depth=float32_to_float16_le"},
				.NoiseLimitations = {"gaussian=rgb_linear_hdr_only"},
				.HookRecords =
					{
						{.Name = "data_capture.rgb_linear_hdr",
						 .SchemaVersion = 1,
						 .NodeKind = "capture",
						 .Required = true,
						 .Channels = {"rgb_linear_hdr"},
						 .Access = "observation",
						 .MutatedFields = {}},
					},
				.MaximumHooks = 14,
				.MaximumConnections = 6,
				.MaximumBatches = 6,
				.MaximumReadbackNodes = 12,
				.MaximumRetainedBytes = 64u * 1024u * 1024u,
				.MaximumPendingPumps = 600,
				.Detail = "ready",
			};
		}

		bool Queue(
			std::string_view, const engine::script::DataCaptureBridgeRequest &, uint64_t &, std::string &
		) override {
			return false;
		}
		bool
		Poll(std::string_view, uint64_t, engine::script::DataCaptureBridgePoll &, std::string &) override {
			return false;
		}
		bool ReadPlane(
			std::string_view,
			uint64_t,
			std::string_view,
			size_t,
			size_t,
			std::vector<std::byte> &,
			std::string &
		) override {
			return false;
		}
		bool Release(std::string_view, uint64_t, std::string &) override {
			return false;
		}
		void Cancel(std::string_view, uint64_t) override {}
	};
}

TEST_CASE("data capture capabilities carry only stable hook facts", "[script][data-capture]") {
	engine::ecs::Store store("capture-capabilities");
	const auto bridge = std::make_shared<CaptureBridge>();
	const engine::script::DataSceneResult result = engine::script::GetCaptureChannels(store, bridge);
	CHECK(std::string_view(result.Status) == "ok");
	REQUIRE(result.Value.Tag == engine::script::ValueTag::Map);

	const auto *schema = Field(result.Value, "schema_version");
	REQUIRE(schema != nullptr);
	CHECK(schema->Text == "data-capture-hooks/v1");
	const auto *hooks = Field(result.Value, "hooks");
	REQUIRE(hooks != nullptr);
	REQUIRE(hooks->Tag == engine::script::ValueTag::Array);
	REQUIRE(hooks->Items.size() == 1);
	const engine::script::ScriptValue &hook = hooks->Items.front();
	CHECK(Field(hook, "name")->Text == "data_capture.rgb_linear_hdr");
	CHECK(Field(hook, "schema_version")->Number == 1.0);
	CHECK(Field(hook, "node_kind")->Text == "capture");
	CHECK(Field(hook, "required")->Boolean);
	const auto *channels = Field(hook, "channels");
	REQUIRE(channels != nullptr);
	REQUIRE(channels->Items.size() == 1);
	CHECK(channels->Items.front().Text == "rgb_linear_hdr");
	const auto *limits = Field(result.Value, "limits");
	REQUIRE(limits != nullptr);
	CHECK(Field(*limits, "maximum_hooks")->Number == 14.0);
	const auto *noise = Field(result.Value, "noise_limitations");
	REQUIRE(noise != nullptr);
	REQUIRE(noise->Items.size() == 1);
	CHECK(noise->Items.front().Text == "gaussian=rgb_linear_hdr_only");
	CHECK(Field(*limits, "maximum_connections")->Number == 6.0);
	CHECK(Field(*limits, "maximum_batches")->Number == 6.0);
	CHECK(Field(*limits, "maximum_readback_nodes")->Number == 12.0);
	CHECK(Field(*limits, "maximum_retained_bytes")->Number == 67'108'864.0);
	CHECK(Field(*limits, "maximum_pending_pumps")->Number == 600.0);
}

TEST_CASE(
	"data scene JSON response budget rejects values the MCP adapter cannot represent",
	"[script][data-capture]"
) {
	using engine::script::DataSceneJsonResponseBudget;
	using engine::script::MAX_DATA_SCENE_JSON_RESPONSE_BYTES;

	engine::script::ScriptValue text{engine::script::ValueTag::String};
	text.Text = "plain";
	size_t bytes = 0;
	REQUIRE(DataSceneJsonResponseBudget(text, bytes));
	CHECK(bytes == 7);
	text.Text = "\"\\";
	REQUIRE(DataSceneJsonResponseBudget(text, bytes));
	CHECK(bytes == 6);
	text.Text = "\b\t\n\f\r";
	REQUIRE(DataSceneJsonResponseBudget(text, bytes));
	CHECK(bytes == 12);
	text.Text.assign(1, '\x01');
	REQUIRE(DataSceneJsonResponseBudget(text, bytes));
	CHECK(bytes == 8);
	text.Text = "\xC3\xA9";
	REQUIRE(DataSceneJsonResponseBudget(text, bytes));
	CHECK(bytes == 4);

	text.Text.assign(MAX_DATA_SCENE_JSON_RESPONSE_BYTES - 2, 'x');
	REQUIRE(DataSceneJsonResponseBudget(text, bytes));
	text.Text.push_back('x');
	CHECK_FALSE(DataSceneJsonResponseBudget(text, bytes));

	engine::script::ScriptValue duplicate{engine::script::ValueTag::Map};
	duplicate.Entries.emplace_back("same", engine::script::ScriptValue{engine::script::ValueTag::Nil});
	duplicate.Entries.emplace_back("same", engine::script::ScriptValue{engine::script::ValueTag::Nil});
	CHECK_FALSE(DataSceneJsonResponseBudget(duplicate, bytes));

	engine::script::ScriptValue nested{engine::script::ValueTag::Nil};
	for (size_t index = 0; index < 17; ++index) {
		engine::script::ScriptValue parent{engine::script::ValueTag::Array};
		parent.Items.push_back(std::move(nested));
		nested = std::move(parent);
	}
	CHECK_FALSE(DataSceneJsonResponseBudget(nested, bytes));
}
