#include "StillExport.hpp"

#include <engine/bake/Image.hpp>
#include <engine/imagegraph/Surface.hpp>
#include <engine/imagegraphexport/GraphExportSession.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>

TEST_SUITE_ID("engine.imagegraphexport.graph_export_session")
TEST_DEPENDS("engine.imagegraph.pendinghostobservations")

namespace {
	using namespace engine::imagegraph;
	using namespace engine::imagegraphexport;
	std::vector<uint8_t> Read(const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		return {std::istreambuf_iterator<char>(file), {}};
	}
	struct Capability : GraphExportSessionHost {
		bool Ready = true, IsPending = false, Float = false, Refuse = false;
		size_t ImageEffects = 0, ShaderAttempts = 0, Cancellations = 0;
		uint32_t DelayWidth = 0;
		Image Expected(uint64_t tick) {
			Image image;
			image.Width = image.Height = 1;
			image.Format = Float ? SurfaceFormat::RGBA32Float : SurfaceFormat::RGBA8Unorm;
			image.Pixels.resize(Float ? 16 : 4);
			REQUIRE(StoreSurfacePixel(
				image,
				0,
				0,
				Float ? SurfacePixel{2.25, -0.5, 0.125, 1.0}
					  : SurfacePixel{double(12 + tick) / 255, 34.0 / 255, 56.0 / 255, 1}
			));
			image.Hash = SurfaceHash(image);
			return image;
		}
		bool Capture(const HostNodeInvocation &call, HostNodeCapture &out, std::string &failure) override {
			if (call.Authored.Type == "pc.hlsl") {
				++ShaderAttempts;
				const auto control = std::find_if(call.Inputs.begin(), call.Inputs.end(), [](const auto &v) {
					return v.Port == "argument_value_0";
				});
				if (DelayWidth) REQUIRE(control != call.Inputs.end());
				if (control != call.Inputs.end()) {
					const auto image =
						std::find_if(call.Images.begin(), call.Images.end(), [](const auto &v) {
							return v.Port == "base_texture";
						});
					REQUIRE(image != call.Images.end());
					REQUIRE(image->Data);
					REQUIRE(std::holds_alternative<double>(control->Data));
					CHECK(std::get<double>(control->Data) == double(image->Data->Width) * 10);
				}
				const bool delayed =
					!DelayWidth ||
					std::any_of(call.Images.begin(), call.Images.end(), [&](const auto &image) {
						return image.Port == "base_texture" && image.Data && image.Data->Width == DelayWidth;
					});
				if (!Ready && delayed) {
					IsPending = true;
					failure = "queued explicit fixture completion";
					return false;
				}
				if (Refuse) {
					failure = "fixture terminal capability refusal";
					return false;
				}
			}
			HostNodeCapture candidate;
			Diagnostic diagnostic;
			uint64_t bytes = 0;
			if (PrepareResolvedHostCapture(call, call.MaximumOperationBytes, candidate, bytes, diagnostic) !=
				Status::Ok) {
				failure = diagnostic.Message;
				return false;
			}
			if (call.Authored.Id == "image") {
				++ImageEffects;
				candidate.Images.push_back({"surface_out", Expected(call.Request.Tick)});
				candidate.Outputs = {{"path", std::string{}}, {"dimension", Vector2{1, 1}}};
			} else {
				REQUIRE(call.Authored.Type == "pc.hlsl");
				const auto image = std::find_if(call.Images.begin(), call.Images.end(), [](const auto &v) {
					return v.Port == "base_texture";
				});
				REQUIRE(image != call.Images.end());
				REQUIRE(image->Data);
				candidate.Images.push_back({"surface", *image->Data});
			}
			out = std::move(candidate);
			return true;
		}
		bool Pending() const noexcept override {
			return IsPending;
		}
		void Cancel() noexcept override {
			++Cancellations;
			IsPending = false;
		}
	};
	struct Fixture {
		inline static std::atomic<uint64_t> NextFixture{0};
		std::filesystem::path Root =
			std::filesystem::temp_directory_path() /
			("atomic-export-session-fixture-" +
			 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
			 std::to_string(NextFixture.fetch_add(1)));
		Document Graph;
		Plan Compiled;
		EvaluationRequest Request;
		EvaluationSnapshot Inputs;
		GraphExportSettings Grants;
		GraphExportGeneration Generation{1, 2, 3, 4, 5};
		Capability Host;
		Fixture(bool floating = false) {
			Host.Float = floating;
			std::error_code error;
			std::filesystem::remove_all(Root, error);
			std::filesystem::create_directories(Root);
			Graph.FormatVersion = 9;
			Graph.Timeline = TimelineSettings{2, 0, 1, "loop", 30};
			Graph.Nodes = {
				{"image", "pc.image", "", {}, {{"path", std::string{}}}},
				{"shader", "pc.hlsl", "", {}, {{"main", std::string{"output.color=1;"}}}},
				{"export",
				 "pc.export",
				 "",
				 {},
				 {{"directory", Root.string()},
				  {"file_name", std::string{"tile"}},
				  {"template", std::string{"%d%n%f"}},
				  {"type", EnumValue{1}},
				  {"format", EnumValue{floating ? 3 : 0}}}}
			};
			Graph.Links = {
				{"image", "surface_out", "shader", "base_texture"}, {"shader", "surface", "export", "surface"}
			};
			Graph.Outputs = {{"preview", "export", "preview"}};
			Diagnostic diagnostic;
			auto status = Compile(Graph, Compiled, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			Request.HostProvider = &Host;
			status = EvaluateNodeInputs(Graph, Compiled, "export", Request, Inputs, diagnostic);
			INFO(diagnostic.Message);
			REQUIRE(status == Status::Ok);
			Grants.Input = Root / "unsaved.graph";
			Grants.Output = Root;
			Grants.OutputId = "preview";
			Host.ImageEffects = Host.ShaderAttempts = 0;
		}
		~Fixture() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
		GraphExportProgress Resume(GraphExportSession &session, bool ready, std::string &failure) {
			Host.Ready = ready;
			Host.IsPending = false;
			return session.Resume(Request, Generation, Host, failure);
		}
		void NoStaging() {
			for (const auto &item : std::filesystem::directory_iterator(Root))
				CHECK_FALSE(item.path().filename().string().starts_with(".graph-export-"));
		}
	};
}
TEST_CASE(
	"range export resumes pending frames without repeating successful host effects",
	"[imagegraph][export-session]"
) {
	Fixture f;
	const auto original = f.Graph;
	GraphExportSession session;
	std::string failure;
	REQUIRE(session.BeginAuthored(f.Graph, f.Inputs, f.Request, f.Grants, "export", f.Generation, failure));
	REQUIRE(session.NextFrame());
	CHECK(session.NextFrame()->Tick == 0);
	for (uint64_t tick = 0; tick < 2; ++tick) {
		CHECK(f.Resume(session, false, failure) == GraphExportProgress::Pending);
		CHECK(session.NextFrame()->Tick == tick);
		CHECK(f.Host.ImageEffects == tick + 1);
		CHECK_FALSE(std::filesystem::exists(f.Root / "tile1.png"));
		const auto resumed = f.Resume(session, true, failure);
		INFO(failure);
		REQUIRE(resumed == GraphExportProgress::Progress);
		CHECK(f.Host.ImageEffects == tick + 1);
	}
	CHECK_FALSE(session.NextFrame());
	const auto completed = f.Resume(session, true, failure);
	INFO(failure);
	REQUIRE(completed == GraphExportProgress::Complete);
	CHECK(f.Host.ImageEffects == 2);
	CHECK(f.Graph == original);
	CHECK(f.Host.ShaderAttempts == 4);
	for (uint64_t tick = 0; tick < 2; ++tick) {
		const auto bytes = Read(f.Root / ("tile" + std::to_string(tick + 1) + ".png"));
		engine::assets::TextureData decoded;
		REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(bytes)), decoded, failure));
		CHECK(decoded.Width == 1);
		CHECK(decoded.Height == 1);
		REQUIRE(decoded.Pixels.size() == 4);
		CHECK(std::to_integer<uint8_t>(decoded.Pixels[0]) == 12 + tick);
		CHECK(std::to_integer<uint8_t>(decoded.Pixels[1]) == 34);
	}
	f.NoStaging();
	CHECK(f.Resume(session, true, failure) == GraphExportProgress::Complete);
	CHECK(f.Host.ShaderAttempts == 4);
}
TEST_CASE(
	"range export cancel and generation changes preserve previous outputs", "[imagegraph][export-session]"
) {
	Fixture f;
	{
		std::ofstream old(f.Root / "tile1.png");
		old << "previous bytes";
	}
	const auto prior = Read(f.Root / "tile1.png");
	GraphExportSession session;
	std::string failure;
	REQUIRE(session.BeginAuthored(f.Graph, f.Inputs, f.Request, f.Grants, "export", f.Generation, failure));
	CHECK(f.Resume(session, false, failure) == GraphExportProgress::Pending);
	SECTION("explicit cancellation") {
		session.Cancel(f.Host);
	}
	SECTION("changed input generation cancels pending device work") {
		++f.Generation.Inputs;
		CHECK(f.Resume(session, true, failure) == GraphExportProgress::Failed);
	}
	SECTION("terminal provider refusal cancels pending device work") {
		f.Host.Refuse = true;
		CHECK(f.Resume(session, true, failure) == GraphExportProgress::Failed);
	}
	CHECK_FALSE(session.NextFrame());
	CHECK(f.Host.Cancellations == 1);
	CHECK(Read(f.Root / "tile1.png") == prior);
	CHECK_FALSE(std::filesystem::exists(f.Root / "tile2.png"));
	f.NoStaging();
}
TEST_CASE(
	"range encoding preserves float pixels and rolls back a later target refusal",
	"[imagegraph][export-session]"
) {
	Fixture f(true);
	GraphExportSession session;
	std::string failure;
	REQUIRE(session.BeginAuthored(f.Graph, f.Inputs, f.Request, f.Grants, "export", f.Generation, failure));
	for (size_t frame = 0; frame < 2; ++frame) {
		const auto status = f.Resume(session, true, failure);
		INFO(failure);
		REQUIRE(status == GraphExportProgress::Progress);
	}
	SECTION("exact typed cache feeds the existing EXR encoder") {
		const auto status = f.Resume(session, true, failure);
		INFO(failure);
		REQUIRE(status == GraphExportProgress::Complete);
		std::vector<uint8_t> expected;
		REQUIRE(engine::imagegraphexport::runner::EncodeStill(".exr", f.Host.Expected(0), expected, failure));
		CHECK(Read(f.Root / "tile1.exr") == expected);
		CHECK(Read(f.Root / "tile2.exr") == expected);
	}
	SECTION("a refused second target leaves first target bytes untouched") {
		{
			std::ofstream previous(f.Root / "tile1.exr");
			previous << "previous float export";
		}
		const auto prior = Read(f.Root / "tile1.exr");
		bool changed = false;
		for (const auto &entry : std::filesystem::recursive_directory_iterator(f.Root)) {
			if (entry.path().filename() != "1-1.cframe") continue;
			auto pixels = Read(entry.path());
			REQUIRE_FALSE(pixels.empty());
			pixels.back() ^= 1;
			std::ofstream corrupt(entry.path(), std::ios::binary | std::ios::trunc);
			corrupt.write(
				reinterpret_cast<const char *>(pixels.data()), static_cast<std::streamsize>(pixels.size())
			);
			changed = true;
		}
		REQUIRE(changed);
		CHECK(f.Resume(session, true, failure) == GraphExportProgress::Failed);
		CHECK(Read(f.Root / "tile1.exr") == prior);
		CHECK_FALSE(std::filesystem::exists(f.Root / "tile2.exr"));
	}
	CHECK(f.Host.ImageEffects == 2);
	CHECK(f.Host.ShaderAttempts == 2);
	f.NoStaging();
}

TEST_CASE(
	"range collection exports each flat image array member with its own dimensions",
	"[imagegraph][export-session]"
) {
	Fixture f;
	auto exportNode = f.Graph.Nodes.back();
	for (auto &value : exportNode.Values)
		if (value.Port == "template") value.Data = std::string{"%d%n%{i+1}_%f"};
	Node array{"array", "pc.array", "", {}, {{"type", EnumValue{1}}}};
	array.DynamicInputs = {{"input_0", ValueType::Image, {}}, {"input_1", ValueType::Image, {}}};
	f.Graph.Nodes = {
		{"a",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{1}}, {"height", int64_t{1}}, {"colour", Colour{255, 0, 0, 255}}}},
		{"b",
		 "image.solid",
		 "",
		 {},
		 {{"width", int64_t{2}}, {"height", int64_t{1}}, {"colour", Colour{0, 255, 0, 255}}}},
		array,
		exportNode
	};
	f.Graph.Links = {
		{"a", "image", "array", "input_0"},
		{"b", "image", "array", "input_1"},
		{"array", "array", "export", "surface"}
	};
	bool asynchronousMembers = false;
	SECTION("pure CPU array members") {}
	SECTION("completed normalized host member survives a later member pending completion") {
		asynchronousMembers = true;
		f.Host.DelayWidth = 2;
		// Normalize each image before the host boundary, then collect completed surfaces.
		for (size_t member = 0; member < 2; ++member) {
			Node shader{
				"shader_" + std::to_string(member),
				"pc.hlsl",
				"",
				{},
				{{"main", std::string{"output.color=gain;"}}}
			};
			shader.DynamicInputs = {
				{"argument_name_0", ValueType::Text, Value{std::string{"gain"}}},
				{"argument_type_0", ValueType::Enum, Value{EnumValue{0}}},
				{"argument_value_0", ValueType::Scalar, Value{double(member + 1) * 10}}
			};
			f.Graph.Links[member].ToNode = shader.Id;
			f.Graph.Links[member].ToPort = "base_texture";
			f.Graph.Links.push_back({shader.Id, "surface", "array", "input_" + std::to_string(member)});
			f.Graph.Nodes.push_back(std::move(shader));
		}
	}
	Diagnostic diagnostic;
	const auto compiled = Compile(f.Graph, f.Compiled, diagnostic);
	INFO(diagnostic.Message);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	REQUIRE(compiled == Status::Ok);
	const auto resolved = EvaluateNodeInputs(f.Graph, f.Compiled, "export", f.Request, f.Inputs, diagnostic);
	INFO(diagnostic.Message);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	REQUIRE(resolved == Status::Ok);
	REQUIRE(f.Inputs.ImageArrays().size() == 1);
	f.Host.ImageEffects = f.Host.ShaderAttempts = 0;
	GraphExportSession session;
	std::string failure;
	REQUIRE(session.BeginAuthored(f.Graph, f.Inputs, f.Request, f.Grants, "export", f.Generation, failure));
	for (size_t frame = 0; frame < 4; ++frame) {
		if (asynchronousMembers) {
			const auto pending = f.Resume(session, false, failure);
			INFO(failure);
			REQUIRE(pending == GraphExportProgress::Pending);
			CHECK(f.Host.ShaderAttempts == frame * 3 + 2);
		}
		const auto status = f.Resume(session, true, failure);
		INFO(failure);
		REQUIRE(status == GraphExportProgress::Progress);
	}
	const auto status = f.Resume(session, true, failure);
	INFO(failure);
	REQUIRE(status == GraphExportProgress::Complete);
	CHECK(f.Host.ImageEffects == 0);
	CHECK(f.Host.ShaderAttempts == (asynchronousMembers ? 12 : 0));
	for (size_t member = 0; member < 2; ++member)
		for (size_t frame = 0; frame < 2; ++frame) {
			const auto bytes = Read(
				f.Root / ("tile" + std::to_string(member + 1) + "_" + std::to_string(frame + 1) + ".png")
			);
			engine::assets::TextureData decoded;
			REQUIRE(engine::bake::ReadImage(std::as_bytes(std::span(bytes)), decoded, failure));
			CHECK(decoded.Width == member + 1);
			CHECK(decoded.Height == 1);
			CHECK(std::to_integer<uint8_t>(decoded.Pixels[0]) == (member ? 0 : 255));
			CHECK(std::to_integer<uint8_t>(decoded.Pixels[1]) == (member ? 255 : 0));
		}
	f.NoStaging();
}

TEST_CASE(
	"range session advances feedback warmup one admitted tick at a time", "[imagegraph][export-session]"
) {
	Fixture f;
	f.Graph.Nodes[0] = {"image", "image.captured", "", {}, {{"source_id", std::string{"feedback:preview"}}}};
	f.Graph.Links[0].FromPort = "image";
	f.Graph.Project.emplace();
	f.Graph.Project->SurfaceWidth = f.Graph.Project->SurfaceHeight = 1;
	f.Graph.Nodes.back().Values.push_back({"custom_range", true});
	f.Graph.Nodes.back().Values.push_back({"frame_range", Vector2{3, 4}});
	std::vector<RequestImageSource> seeds{{"feedback:preview", Image{1, 1, {0, 0, 0, 0}}}};
	f.Request.ImageSources = seeds;
	Diagnostic diagnostic;
	const auto compiled = Compile(f.Graph, f.Compiled, diagnostic);
	INFO(diagnostic.Message);
	INFO(diagnostic.NodeId);
	INFO(diagnostic.Port);
	REQUIRE(compiled == Status::Ok);
	const auto prepared = EvaluateNodeInputs(f.Graph, f.Compiled, "export", f.Request, f.Inputs, diagnostic);
	INFO(diagnostic.Message);
	REQUIRE(prepared == Status::Ok);
	SECTION("one high-clock temporal frame is refused before staging or publication") {
		for (auto &value : f.Graph.Nodes.back().Values)
			if (value.Port == "frame_range") value.Data = Vector2{4097, 4097};
		REQUIRE(Compile(f.Graph, f.Compiled, diagnostic) == Status::Ok);
		REQUIRE(
			EvaluateNodeInputs(f.Graph, f.Compiled, "export", f.Request, f.Inputs, diagnostic) == Status::Ok
		);
		const auto previous = f.Root / "tile4097.png";
		{
			std::ofstream file(previous);
			file << "previous destination";
		}
		const auto before = Read(previous);
		GraphExportSession rejected;
		std::string failure;
		CHECK_FALSE(
			rejected.BeginAuthored(f.Graph, f.Inputs, f.Request, f.Grants, "export", f.Generation, failure)
		);
		CHECK(failure.find("temporal warmup") != std::string::npos);
		CHECK(Read(previous) == before);
		CHECK_FALSE(rejected.NextFrame());
		f.NoStaging();
		return;
	}
	SECTION("warmup produces exactly the selected frame files") {}
	f.Host.ShaderAttempts = 0;
	GraphExportSession session;
	std::string failure;
	REQUIRE(session.BeginAuthored(f.Graph, f.Inputs, f.Request, f.Grants, "export", f.Generation, failure));
	for (uint64_t tick = 0; tick < 4; ++tick) {
		REQUIRE(session.NextFrame());
		CHECK(session.NextFrame()->Tick == tick);
		const auto pending = f.Resume(session, false, failure);
		INFO(failure);
		REQUIRE(pending == GraphExportProgress::Pending);
		CHECK(session.NextFrame()->Tick == tick);
		const auto completed = f.Resume(session, true, failure);
		INFO(failure);
		REQUIRE(completed == GraphExportProgress::Progress);
		CHECK(f.Host.ShaderAttempts == (tick + 1) * 2);
	}
	CHECK_FALSE(session.NextFrame());
	const auto published = f.Resume(session, true, failure);
	INFO(failure);
	REQUIRE(published == GraphExportProgress::Complete);
	CHECK_FALSE(std::filesystem::exists(f.Root / "tile1.png"));
	CHECK_FALSE(std::filesystem::exists(f.Root / "tile2.png"));
	CHECK(std::filesystem::exists(f.Root / "tile3.png"));
	CHECK(std::filesystem::exists(f.Root / "tile4.png"));
	CHECK(f.Host.ShaderAttempts == 8);
	f.NoStaging();
}
