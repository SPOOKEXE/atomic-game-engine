#include "ImageGraphComposerCadence.hpp"
#include "ImageGraphComposerExports.hpp"
#include "ImageGraphHost.hpp"

#include <engine/scripthost/ComposerLua.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

TEST_SUITE_ID("studio.imagegraph.composer_lua")
TEST_DEPENDS("studio.imagegraph")
TEST_DEPENDS("engine.scripthost.composer_lua")
namespace {
	using namespace engine::imagegraph;
	struct CountedLua final : ComposerLuaHost {
		std::unique_ptr<ComposerLuaHost> Host = engine::script::MakeComposerLuaHost();
		size_t Calls = 0;
		bool Capture(
			const HostNodeInvocation &invocation, HostNodeCapture &capture, std::string &failure
		) override {
			++Calls;
			return Host->Capture(invocation, capture, failure);
		}
		bool PcxMessages(
			std::string_view node, std::span<const PcxMessage> messages, std::string &failure
		) override {
			return Host->PcxMessages(node, messages, failure);
		}
		std::vector<ComposerLuaMessage> TakeMessages() override {
			return Host->TakeMessages();
		}
		void Reset() override {
			Host->Reset();
		}
	};
	Document Graph() {
		Document doc;
		doc.FormatVersion = 9;
		doc.Nodes = {
			{"lua",
			 "pc.lua_compute",
			 {},
			 {},
			 {{"lua_code", std::string("counter=(counter or 0)+1 print(counter) return counter")},
			  {"function_name", std::string("pendingArgument")},
			  {"execute_on_frame", true}}},
			{"hlsl", "pc.hlsl", {}, {}, {}},
			{"base",
			 "image.solid",
			 {},
			 {},
			 {{"width", int64_t{2}}, {"height", int64_t{2}}, {"colour", Colour{1, 2, 3, 255}}}},
			{"export", "pc.export", {}, {}, {{"type", EnumValue{0}}, {"export_on_update", true}}}
		};
		doc.Nodes[1].DynamicInputs = {
			{"argument_name_0", ValueType::Text, std::string("gain")},
			{"argument_type_0", ValueType::Enum, EnumValue{0}},
			{"argument_value_0", ValueType::Scalar, 0.0}
		};
		doc.Links = {
			{"lua", "return_value", "hlsl", "argument_value_0"},
			{"lua", "return_value", "export", "sequence_begin"},
			{"base", "image", "export", "surface"}
		};
		doc.Outputs = {{"out", "hlsl", "surface"}};
		return doc;
	}
} // namespace
TEST_CASE(
	"Pending shader input receipts survive genuine Lua export cone "
	"interleaving",
	"[studio][composer_cadence][composer_lua]"
) {
	using namespace studio::detail;
	auto doc = Graph();
	Plan plan;
	Diagnostic error;
	REQUIRE(Compile(doc, plan, error) == Status::Ok);
	CountedLua lua;
	ImageGraphHost host;
	host.Lua = &lua;
	EvaluationRequest request;
	request.HostProvider = &host;
	ImageGraphComposerCadence cadence;
	ImageGraphComposerCadence::Identity identity{engine::core::Name("panel-world"), "out", 3, 4, 0};
	ImageGraphObservations observations;
	std::tm calendar{};
	observations.Capture(3, {12}, "project", 12, calendar);
	const auto frame = cadence.Begin(identity, {12}, observations);
	host.LuaReceipts.Begin(frame);
	const auto prepare = [&](std::string_view node, FrameTime tick, bool frozen) {
		EvaluationSnapshot snapshot;
		REQUIRE(SetFrameTime(request, tick));
		const bool previous = host.LuaReceipts.Active;
		host.LuaReceipts.Active = frozen;
		const auto result = EvaluateNodeInputs(doc, plan, node, request, snapshot, error);
		host.LuaReceipts.Active = previous;
		INFO(error.Message);
		REQUIRE(result == Status::Ok);
		const auto port = node == "hlsl" ? "argument_value_0" : "sequence_begin";
		const auto value =
			std::find_if(snapshot.Values().begin(), snapshot.Values().end(), [&](const auto &input) {
				return input.Port == port;
			});
		REQUIRE(value != snapshot.Values().end());
		return value->Data;
	};
	CHECK(std::get<double>(prepare("hlsl", frame, true)) == 1.0);
	CHECK(lua.Calls == 1);
	CHECK(lua.TakeMessages().size() == 1);
	EvaluationSnapshot luaControls;
	REQUIRE(EvaluateNodeInputs(doc, plan, "lua", request, luaControls, error) == Status::Ok);
	CHECK(lua.Calls == 1);
	HostNodeCapture retained;
	retained.Authored.Id = "previous-output";
	retained.Outputs = {{"previous", 42.0}};
	const auto previousBytes = ImageGraphCaptureCloneBytes(retained);
	REQUIRE(previousBytes);
	const auto receiptBytes = host.LuaReceipts.Bytes;
	std::vector<AuthoredValue> controlValues;
	for (const auto &value : luaControls.Values())
		controlValues.push_back({value.Port, value.Data});
	HostNodeInvocation shortInvocation{
		doc.Nodes[0], request, controlValues, {}, host.RetainedBytes + receiptBytes + *previousBytes - 1
	};
	std::string failure;
	{
		ImageGraphLuaReceiptScope frozen(host.LuaReceipts);
		CHECK_FALSE(host.Capture(shortInvocation, retained, failure));
	}
	CHECK(retained.Authored.Id == "previous-output");
	REQUIRE(retained.Outputs.size() == 1);
	CHECK(std::get<double>(retained.Outputs[0].Data) == 42.0);
	CHECK(host.LuaReceipts.Bytes == receiptBytes);
	CHECK(lua.Calls == 1);
	cadence.Pending();
	// A real newer-frame capability cone changes the shared VM's latest cache.
	CHECK(prepare("export", {13}, false) == Value{int64_t{2}});
	CHECK(lua.TakeMessages().size() == 1);
	const auto callsAfterExport = lua.Calls;
	for (uint64_t authoritative = 14; authoritative <= 17; ++authoritative) {
		const auto held = cadence.Begin(identity, {authoritative}, observations);
		CHECK(held == frame);
		CHECK(std::get<double>(prepare("hlsl", held, true)) == 1.0);
		CHECK(lua.Calls == callsAfterExport);
		CHECK(lua.TakeMessages().empty());
	}
	REQUIRE(cadence.Complete(frame));
	CHECK(prepare("export", frame, true) == Value{int64_t{1}});
	CHECK(lua.Calls == callsAfterExport);
	CHECK(lua.TakeMessages().empty());
	const auto next = cadence.Begin(identity, {18}, observations);
	host.LuaReceipts.Begin(next);
	CHECK(std::get<double>(prepare("hlsl", next, true)) == 3.0);
	CHECK(lua.TakeMessages().size() == 1);
	// Reloaded audio/input generation cancels immutable receipts before next
	// capture.
	++identity.InputRevision;
	CHECK_FALSE(cadence.Matches(identity));
	cadence.Cancel();
	host.LuaReceipts.Clear();
	cadence.Begin(identity, {19}, observations);
	host.LuaReceipts.Begin({19});
	CHECK(std::get<double>(prepare("hlsl", {19}, true)) == 4.0);
	CHECK(lua.TakeMessages().size() == 1);
}
TEST_CASE(
	"Pending exports retain each observed frame and refuse queue "
	"overflow atomically",
	"[studio][composer_cadence][composer_lua]"
) {
	using namespace studio::detail;
	ImageGraphComposerExports exports;
	std::string failure;
	std::tm calendar{};
	for (uint64_t frame = 12; frame < 12 + ImageGraphComposerExports::MaximumObservations; ++frame) {
		ImageGraphComposerExports::Observation observation;
		observation.Revision = 3;
		observation.InputRevision = 4;
		REQUIRE(studio::SetImageGraphAuthorFrame(observation.Playback, {frame}));
		observation.Pcx.Capture(3, {frame}, "queued", double(frame), calendar);
		REQUIRE(exports.Admit(observation, failure));
	}
	const auto count = exports.Pending.size();
	ImageGraphComposerExports::Observation overflow;
	overflow.Revision = 3;
	overflow.InputRevision = 4;
	REQUIRE(studio::SetImageGraphAuthorFrame(overflow.Playback, {100}));
	CHECK_FALSE(exports.Admit(overflow, failure));
	CHECK(exports.Pending.size() == count);
	REQUIRE_FALSE(failure.empty());
	for (size_t i = 0; i < exports.Pending.size(); ++i) {
		CHECK(
			studio::GetImageGraphFrame(exports.Pending[i].Playback) == engine::imagegraph::FrameTime{12 + i}
		);
		CHECK(std::get<double>(exports.Pending[i].Pcx.Values.front().Data) == double(12 + i));
	}
	exports.Invalidate(3, 5);
	CHECK(exports.Pending.empty());
	overflow.InputRevision = 5;
	REQUIRE(exports.Admit(overflow, failure));
	CHECK(exports.Pending.size() == 1);
}
TEST_CASE(
	"Dense valid PCX controls refuse excess copied storage before growing the export queue",
	"[studio][composer_cadence][composer_lua]"
) {
	using namespace studio::detail;
	ImageGraphComposerExports queue;
	ImageGraphComposerExports::Observation observation;
	observation.Revision = 3;
	observation.InputRevision = 4;
	std::tm calendar{};
	observation.Pcx.Capture(3, {12}, "scene", 12, calendar);
	for (auto &control : observation.Pcx.Values)
		control.Port.reserve(ImageGraphComposerExports::MaximumBytes / observation.Pcx.Values.size());
	std::string failure;
	const auto oldCapacity = queue.Pending.capacity();
	CHECK_FALSE(queue.Admit(observation, failure));
	CHECK(queue.Pending.empty());
	CHECK(queue.Pending.capacity() == oldCapacity);
	CHECK_FALSE(failure.empty());
	for (const auto &control : observation.Pcx.Values)
		CHECK(std::holds_alternative<double>(control.Data));
}
