#include <engine/imagegraphexport/GraphCommandHost.hpp>
#include <engine/parallel/Process.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>
#include <thread>
TEST_SUITE_ID("engine.imagegraphexport.graph_command_host")
TEST_DEPENDS("engine.parallel.process")
TEST_DEPENDS("engine.imagegraph.host_capture")
namespace {
	struct Scratch {
		std::filesystem::path Root = std::filesystem::temp_directory_path() / "atomic-graph-command-test";
		Scratch() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
			std::filesystem::create_directory(Root, error);
		}
		~Scratch() {
			std::error_code error;
			std::filesystem::remove_all(Root, error);
		}
	};
}
TEST_CASE(
	"Explicit process grants bind source script and literal argv in child cwd",
	"[assetc][imagegraph][external-codec]"
) {
	using namespace engine::imagegraph;
	const std::filesystem::path python = "/usr/bin/python3";
	if (!std::filesystem::is_regular_file(python)) SKIP("independent Python command is unavailable");
	Scratch scratch;
	const std::array<engine::imagegraphexport::GraphProcessGrant, 1> grants{
		{{"shell",
		  python,
		  "approved text",
		  {"-c",
		   "from pathlib import Path; import sys; Path('receipt').write_text(sys.argv[1])",
		   "literal $() ; text"},
		  scratch.Root,
		  std::chrono::seconds(2)}}
	};
	engine::imagegraphexport::GraphCommandHost host(grants, {});
	Node node;
	node.Id = "shell";
	node.Type = "pc.shell";
	EvaluationRequest request;
	std::array<AuthoredValue, 2> controls{
		{{"path", python.string()}, {"script", std::string{"approved text"}}}
	};
	HostNodeCapture result;
	std::string failure;
	REQUIRE(host.Capture({node, request, controls, {}, 1048576}, result, failure));
	std::ifstream receipt(scratch.Root / "receipt");
	std::string text;
	std::getline(receipt, text);
	CHECK(text == "literal $() ; text");
	CHECK(result.Outputs.empty());
	CHECK(result.Authored == node);
	controls[1].Data = std::string{"ungranted text"};
	CHECK_FALSE(host.Capture({node, request, controls, {}, 1048576}, result, failure));
}
TEST_CASE(
	"Local HTTP host serves source GET POST and enforces byte limits", "[assetc][imagegraph][external-codec]"
) {
	using namespace engine::imagegraph;
	const std::filesystem::path python = "/usr/bin/python3", curl = "/usr/bin/curl";
	if (!std::filesystem::is_regular_file(python) || !std::filesystem::is_regular_file(curl))
		SKIP("independent local HTTP tools are unavailable");
	Scratch scratch;
	const std::string server = R"(from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*args): pass
 def do_GET(self):
  body=b'x'*1000 if self.path=='/big' else b'hello'
  self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
 def do_POST(self):
  body=self.rfile.read(int(self.headers['Content-Length']))
  self.send_response(200); self.send_header('Content-Length',str(len(body))); self.end_headers(); self.wfile.write(body)
server=HTTPServer(('127.0.0.1',0),Handler)
Path('port').write_text(str(server.server_port))
server.serve_forever()
)";
	engine::parallel::Process process;
	REQUIRE(process.Start(python, {"-c", server}, scratch.Root));
	const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	std::string port;
	while (std::chrono::steady_clock::now() < until) {
		// write_text creates the file before its contents are ready.
		std::ifstream portFile(scratch.Root / "port");
		portFile >> port;
		if (!port.empty()) break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	REQUIRE_FALSE(port.empty());
	const std::string address = "http://127.0.0.1:" + port + "/text";
	std::array<engine::imagegraphexport::GraphHttpGrant, 1> grants{
		{{"http", address, false, "", curl, 10000, std::chrono::seconds(2)}}
	};
	engine::imagegraphexport::GraphCommandHost host({}, grants);
	Node node;
	node.Id = "http";
	node.Type = "pc.http_request";
	EvaluationRequest request;
	std::array<AuthoredValue, 4> controls{
		{{"address", address},
		 {"type", EnumValue{0}},
		 {"content", std::string{}},
		 {"attribute_max_file_size", 10000.0}}
	};
	HostNodeCapture capture;
	std::string failure;
	REQUIRE(host.Capture({node, request, controls, {}, 1048576}, capture, failure));
	CHECK(std::get<std::string>(capture.Outputs[0].Data) == "hello");
	grants[0].Post = true;
	grants[0].Content = "a=literal&b=spaces";
	controls[1].Data = EnumValue{1};
	controls[2].Data = grants[0].Content;
	REQUIRE(host.Capture({node, request, controls, {}, 1048576}, capture, failure));
	CHECK(std::get<std::string>(capture.Outputs[0].Data) == grants[0].Content);
	grants[0].Address = "http://127.0.0.1:" + port + "/big";
	grants[0].Post = false;
	grants[0].Content.clear();
	controls[0].Data = grants[0].Address;
	controls[1].Data = EnumValue{0};
	controls[2].Data = std::string{};
	controls[3].Data = 10.0;
	CHECK_FALSE(host.Capture({node, request, controls, {}, 1048576}, capture, failure));
	controls[0].Data = address;
	CHECK_FALSE(host.Capture({node, request, controls, {}, 1048576}, capture, failure));
	REQUIRE(process.Kill());
	(void)process.Wait();
}

TEST_CASE(
	"Command grant parser preserves exact literal data and rejects malformed records", "[assetc][imagegraph]"
) {
	std::vector<engine::imagegraphexport::GraphProcessGrant> processes;
	std::vector<engine::imagegraphexport::GraphHttpGrant> requests;
	std::string failure;
	REQUIRE(
		engine::imagegraphexport::ReadGraphCommandGrants(
			"imagegraph-host-grants 1\nprocess \"run\" \"/usr/bin/python3\" \"/tmp\" \"approved\" 2000 2 "
			"\"-c\" \"literal $()\"\nhttp \"fetch\" \"http://127.0.0.1/text\" 1 \"a=b\" \"/usr/bin/curl\" "
			"10000 2000\n",
			processes,
			requests,
			failure
		)
	);
	REQUIRE(processes.size() == 1);
	CHECK(processes[0].Arguments[1] == "literal $()");
	REQUIRE(requests.size() == 1);
	CHECK(requests[0].Post);
	CHECK(requests[0].Content == "a=b");
	CHECK_FALSE(
		engine::imagegraphexport::ReadGraphCommandGrants(
			"imagegraph-host-grants 2", processes, requests, failure
		)
	);
	CHECK(processes.size() == 1);
	CHECK_FALSE(
		engine::imagegraphexport::ReadGraphCommandGrants(
			"imagegraph-host-grants 1 process \"n\" \"/bin/true\" \"/tmp\" \"\" 18446744073709551615 0",
			processes,
			requests,
			failure
		)
	);
	CHECK_FALSE(
		engine::imagegraphexport::ReadGraphCommandGrants(
			"imagegraph-host-grants 1 process \"n\" \"relative\" \"/tmp\" \"\" 2000 0",
			processes,
			requests,
			failure
		)
	);
}

TEST_CASE("Explicit clock grants produce bound observations and can be replayed", "[assetc][imagegraph]") {
	using namespace engine::imagegraph;
	std::vector<engine::imagegraphexport::GraphProcessGrant> processes;
	std::vector<engine::imagegraphexport::GraphHttpGrant> requests;
	std::vector<std::string> clocks;
	std::string failure;
	REQUIRE(
		engine::imagegraphexport::ReadGraphCommandGrants(
			"imagegraph-host-grants 1\nclock \"clock\"\n", processes, requests, failure, &clocks
		)
	);
	engine::imagegraphexport::GraphCommandHost host(
		processes,
		requests,
		engine::assets::ContentPolicy::Process(engine::assets::ContentVerb::Handle),
		clocks
	);
	Node node;
	node.Id = "clock";
	node.Type = "pc.datetime_get";
	std::array<AuthoredValue, 1> inputs{{{"format", std::string("%y-%m-%dT%h:%n:%s")}}};
	EvaluationRequest request;
	request.Tick = 17;
	HostNodeCapture capture;
	REQUIRE(host.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	CHECK(capture.Tick == 17);
	const auto &text = std::get<std::string>(capture.Outputs[0].Data);
	CHECK(text.size() == 19);
	CHECK(text[4] == '-');
	CHECK(text[10] == 'T');
	Document document;
	document.Nodes.push_back(node);
	document.Outputs.push_back({"date", "clock", "data"});
	Plan plan;
	Diagnostic diagnostic;
	REQUIRE(Compile(document, plan, diagnostic) == Status::Ok);
	EvaluationSnapshot snapshot;
	REQUIRE(EvaluateNodeInputs(document, plan, "clock", request, snapshot, diagnostic) == Status::Ok);
	std::vector<AuthoredValue> resolved;
	for (const auto &value : snapshot.Values())
		resolved.push_back({value.Port, value.Data});
	REQUIRE(host.Capture({document.Nodes[0], request, resolved, {}, 1048576}, capture, failure));
	request.HostCaptures = std::span<const HostNodeCapture>(&capture, 1);
	EvaluatedValue replay;
	REQUIRE(EvaluateValue(document, plan, "date", request, replay, diagnostic) == Status::Ok);
	CHECK(std::get<std::string>(replay.Data) == std::get<std::string>(capture.Outputs[0].Data));
	clocks.clear();
	engine::imagegraphexport::GraphCommandHost refused(processes, requests);
	CHECK_FALSE(refused.Capture({node, request, inputs, {}, 1048576}, capture, failure));
	CHECK_FALSE(
		engine::imagegraphexport::ReadGraphCommandGrants(
			"imagegraph-host-grants 1\nclock \"x\"\nclock \"x\"\n", processes, requests, failure, &clocks
		)
	);
}
