#include <engine/bake/SpriteCache.hpp>
#include <engine/imagegraph/Catalogue.hpp>
#include <engine/imagegraph/StatefulReplay.hpp>
#include <engine/imagegraphio/PxcxStructureEdit.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("engine.imagegraphio.pxcx_particle2d")
namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphio;
	using Json = nlohmann::ordered_json;
	Json Fixed(Json value) {
		return {{"r", {{"d", std::move(value)}}}, {"future_input", 19}};
	}
	Json Wire(const char *id, int index = 0) {
		return {{"from_node", id}, {"from_index", index}, {"from_tag", 0}};
	}
	Json Record(const char *id, const char *type, Json inputs) {
		return {
			{"id", id},
			{"type", type},
			{"x", 0},
			{"y", 0},
			{"inputs", std::move(inputs)},
			{"future_node", "keep"}
		};
	}
	Json Project() {
		engine::bake::SpriteCacheFrame sprite{2, 2, std::vector<uint8_t>(16, 255)};
		std::string cache, failure;
		REQUIRE(
			engine::bake::WriteSpriteCache(
				std::span<const engine::bake::SpriteCacheFrame>(&sprite, 1),
				engine::bake::SpriteCacheLayout::Rgba8TopDown,
				cache,
				failure,
				Limits::MaximumEvaluationBytes,
				engine::bake::SpriteCacheShape::Sprite
			)
		);
		const auto hash = engine::bake::SpriteCacheDataHash(cache);
		REQUIRE(hash);
		auto image = Record("sprite", "Node_Image", Json::array({Fixed("missing.png")}));
		image["attri"] = {{"cache_use", true}, {"cache_data", cache}};
		image["atomic_game_engine"] = {
			{"version", 1},
			{"sprite_cache",
			 {{"layout", "rgba8-top-down"}, {"data_hash", std::string(hash->data(), hash->size())}}}
		};
		Json controls = std::vector<Json>(84, Json::object());
		controls[0] = Wire("sprite");
		controls[1] = Fixed(0);
		controls[2] = Fixed(Json::array({1, 1}));
		controls[4] = Fixed(4);
		controls[5] = Fixed(Json::array({2, 2}));
		controls[10] = Fixed(Json::array({1, 1, 1, 1}));
		controls[16] = Fixed(1);
		controls[17] = Fixed(Json::array({1, 1}));
		controls[18] = Fixed(Json::array({0, 0}));
		controls[21] = Fixed(false);
		controls[24] = Fixed(0);
		controls[27] = Fixed(true);
		controls[32] = Fixed(17);
		controls[51] = Fixed(1);
		controls[62] = Fixed(Json::array({Json::array({4, 4})}));
		controls[65] = Fixed(0);
		controls[71] = Fixed(Json::array({8, 8}));
		controls[71]["attri"] = {{"use_project_dimension", 0}};
		controls[72] = Fixed(false);
		controls[77] = Fixed(true);
		auto emitter = Record("emitter", "Node_Particle", controls);
		auto scope = Record(
			"scope",
			"Node_VFX_Group_Inline",
			Json::array({Fixed(false), Fixed(Json::array({8, 8})), Fixed(0)})
		);
		scope["inputs"][1]["attri"] = {{"use_project_dimension", 0}};
		auto renderer = Record(
			"renderer",
			"Node_VFX_Renderer",
			Json::array(
				{Fixed(Json::array({8, 8})), Fixed(false), Fixed(0), Fixed(4), Fixed(0), Wire("emitter", 1)}
			)
		);
		renderer["group"] = "scope";
		return {
			{"attributes", {{"surface_dimension", Json::array({8, 8})}}},
			{"animator", {{"frames_total", 4}, {"playback", 0}, {"framerate", 24}}},
			{"future_project", 23},
			{"nodes", Json::array({image, emitter, scope, renderer})}
		};
	}
	PxcxImport Reimport(const std::vector<std::byte> &bytes) {
		engine::bake::PxcxArchive archive;
		std::string failure;
		REQUIRE(engine::bake::ReadPxcx(bytes, archive, failure));
		PxcxImport imported;
		const auto accepted = ImportPxcxImageGraph(archive, imported, failure);
		INFO(failure);
		REQUIRE(accepted);
		return imported;
	}
	PxcxImport Imported() {
		engine::bake::PxcxArchive archive;
		archive.MetadataNumber = 121092;
		archive.MetadataText = "1.22.10.201";
		archive.GraphJson = Project().dump() + '\0';
		std::vector<std::byte> bytes;
		std::string failure;
		REQUIRE(engine::bake::WritePxcx(archive, bytes, failure));
		return Reimport(bytes);
	}
	Node &Native(Document &document, std::string_view id) {
		auto node = std::find_if(document.Nodes.begin(), document.Nodes.end(), [&](const auto &candidate) {
			return candidate.Id == id;
		});
		REQUIRE(node != document.Nodes.end());
		return *node;
	}
	StatefulOutputEvaluationResult
	Frame(Document document, uint64_t tick, const DataReplayState *previous = nullptr) {
		document.Outputs = {
			{"surface", "emitter", "surface_out"},
			{"particles", "emitter", "data"},
			{"vfx", "renderer", "surface_out"}
		};
		Plan plan;
		Diagnostic diagnostic;
		const auto compiled = Compile(document, plan, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(compiled == Status::Ok);
		EvaluationRequest request;
		request.Tick = tick;
		request.DataReplay = previous;
		HostNodeCapture capture;
		REQUIRE(PrepareHostCapture(document, plan, "sprite", request, capture, diagnostic) == Status::Ok);
		const auto &sprite = Native(document, "sprite");
		const std::string *cache = nullptr;
		for (const auto &property : sprite.SourceProperties)
			if (property.Port == "cache_data") cache = std::get_if<std::string>(&property.Data);
		REQUIRE(cache);
		std::vector<engine::bake::SpriteCacheFrame> frames;
		std::string failure;
		REQUIRE(
			engine::bake::ReadSpriteCache(
				*cache,
				engine::bake::SpriteCacheLayout::Rgba8TopDown,
				frames,
				failure,
				Limits::MaximumEvaluationBytes,
				engine::bake::SpriteCacheShape::Sprite
			)
		);
		REQUIRE(frames.size() == 1);
		const auto *entry = FindCatalogueEntry("pc.image");
		REQUIRE(entry);
		capture.Images = {
			{std::string(entry->Outputs[0].Id), Image{frames[0].Width, frames[0].Height, frames[0].Rgba}}
		};
		request.HostCaptures = std::span<const HostNodeCapture>(&capture, 1);
		const std::array<std::string, 3> outputs{"surface", "particles", "vfx"};
		StatefulOutputEvaluationResult result;
		const auto status = EvaluateStatefulOutputs(document, plan, outputs, request, result, diagnostic);
		INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
		REQUIRE(status == Status::Ok);
		return result;
	}
	const StatefulNamedOutput &Output(const StatefulOutputEvaluationResult &frame, std::string_view id) {
		const auto found = std::find_if(frame.Outputs.begin(), frame.Outputs.end(), [&](const auto &output) {
			return output.Id == id;
		});
		REQUIRE(found != frame.Outputs.end());
		return *found;
	}
	void Pixels(const StatefulOutputEvaluationResult &frame, uint32_t centerX = 4) {
		std::vector<uint8_t> expected(8 * 8 * 4);
		for (uint32_t y : {3u, 4u})
			for (uint32_t x : {centerX - 1, centerX})
				for (uint32_t channel = 0; channel < 4; ++channel)
					expected[(y * 8 + x) * 4 + channel] = 255;
		for (const auto id : {"surface", "vfx"}) {
			CAPTURE(id);
			const auto *image = std::get_if<Image>(&Output(frame, id).Output);
			REQUIRE(image);
			CHECK(image->Width == 8);
			CHECK(image->Height == 8);
			CHECK(image->Pixels == expected);
		}
		const auto *value = std::get_if<EvaluatedValue>(&Output(frame, "particles").Output);
		REQUIRE(value);
		const auto *array = std::get_if<ArrayValue>(&value->Data);
		REQUIRE(array);
		CHECK(array->ElementType == ValueType::Particle);
		size_t active = 0;
		for (const auto &item : array->Elements) {
			const auto *particle = std::get_if<ParticleValue>(&item);
			REQUIRE(particle);
			REQUIRE(particle->Data);
			if (particle->Data->State.Active) {
				++active;
				CHECK(particle->Data->OriginNodeId == "emitter");
				CHECK(particle->Data->State.Position == std::array<double, 2>{double(centerX), 4});
			}
		}
		CHECK(active == 1);
	}
}

TEST_CASE(
	"PXC Particle emits actual owned data and replay surfaces through a VFX renderer",
	"[imagegraphio][pxcx_particle2d]"
) {
	const auto imported = Imported();
	auto graph = imported.Graph;
	CHECK(Native(graph, "emitter").Type == "pc.particle");
	CHECK(Native(graph, "renderer").Type == "pc.vfx_renderer");
	CHECK(Native(graph, "scope").Type == "pc.vfx_group_inline");
	const auto first = Frame(graph, 0);
	Pixels(first);
	REQUIRE_FALSE(first.Data.Entries.empty());
	const auto second = Frame(graph, 1, &first.Data);
	Pixels(second);
	CHECK(second.Data != first.Data);
	const auto cold = Frame(graph, 0);
	Pixels(cold);
	CHECK(cold.Data == first.Data);
}

TEST_CASE(
	"Edited PXC Particle saves authored spawn controls and reopens at constructor state",
	"[imagegraphio][pxcx_particle2d]"
) {
	const auto imported = Imported();
	auto desired = imported.Graph;
	Diagnostic diagnostic;
	REQUIRE(Migrate(desired, diagnostic) == Status::Ok);
	auto &emitter = Native(desired, "emitter");
	bool changed = false;
	for (auto &input : emitter.Values)
		if (input.Port == "spawn_data") {
			ArrayValue points;
			points.ElementType = ValueType::Vector2;
			points.Elements = {Vector2{6, 4}};
			input.Data = points;
			changed = true;
		}
	REQUIRE(changed);
	emitter.Position = {12, -4};
	Document native;
	REQUIRE(Read(Write(desired), native, diagnostic) == Status::Ok);
	CHECK(native == desired);
	const auto first = Frame(native, 0);
	Pixels(first, 6);
	const auto second = Frame(native, 1, &first.Data);
	Pixels(second, 6);
	std::vector<std::byte> bytes;
	const auto accepted = WritePxcxProjection(imported, native, {}, bytes, diagnostic);
	INFO(diagnostic.NodeId << ':' << diagnostic.Port << ':' << diagnostic.Message);
	REQUIRE(accepted);
	auto reopened = Reimport(bytes);
	REQUIRE(Migrate(reopened.Graph, diagnostic) == Status::Ok);
	CHECK(reopened.Graph == native);
	const auto cold = Frame(reopened.Graph, 0);
	Pixels(cold, 6);
	CHECK(cold.Data == first.Data);
	CHECK(cold.Data != second.Data);
	const auto saved = Json::parse(reopened.Source.GraphJson.c_str());
	CHECK(saved["future_project"] == 23);
	CHECK(saved["nodes"][1]["future_node"] == "keep");
	CHECK(saved["nodes"][1]["inputs"][62]["future_input"] == 19);
	std::vector<std::byte> again;
	REQUIRE(WritePxcxProjection(reopened, reopened.Graph, {}, again, diagnostic));
	CHECK(again == bytes);
}
