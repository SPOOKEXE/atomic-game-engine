#include <engine/ecs/Store.hpp>
#include <engine/scene/Part.hpp>
#include <engine/scene/Registration.hpp>
#include <engine/script/ComputeBridge.hpp>
#include <engine/scripthost/Runtime.hpp>
#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

TEST_SUITE_ID("engine.scripthost.computegraphservice")
TEST_DEPENDS("engine.computegraph.stock")

namespace {
	// Controlled copied endpoint checks VM marshalling only. It performs no GPU execution.
	struct Endpoint final : engine::script::ComputeBridge {
		engine::computegraph::Owner Owner{};
		engine::computegraph::Program Defined{};
		engine::computegraph::Request Submitted{};
		unsigned Detached = 0;
		unsigned Definitions = 0;
		const engine::computegraph::Owner &Identity() const override {
			return Owner;
		}
		engine::computegraph::Status Define(
			std::string name, engine::computegraph::Program program, uint64_t &ticket, std::string &
		) override {
			CHECK(name == "test");
			++Definitions;
			Defined = std::move(program);
			ticket = 71;
			return engine::computegraph::Status::Ok;
		}
		engine::computegraph::Status Submit(
			std::string name, engine::computegraph::Request request, uint64_t &ticket, std::string &
		) override {
			CHECK(name == "test");
			Submitted = std::move(request);
			ticket = 72;
			return engine::computegraph::Status::Ok;
		}
		engine::computegraph::Status Poll(uint64_t ticket, engine::script::ComputePoll &out) override {
			CHECK(ticket == 72);
			out.Ready = true;
			out.Outputs = {{"packed", engine::computegraph::ElementType::U32, 1, 4}};
			return engine::computegraph::Status::Ok;
		}
		engine::computegraph::Status Read(
			uint64_t ticket,
			std::string_view name,
			uint64_t offset,
			size_t maximum,
			std::vector<std::byte> &out
		) override {
			CHECK(ticket == 72);
			CHECK(name == "packed");
			CHECK(offset == 0);
			CHECK(maximum == 4);
			uint32_t value = 84;
			out.resize(4);
			std::memcpy(out.data(), &value, 4);
			return engine::computegraph::Status::Ok;
		}
		bool Cancel(uint64_t ticket) override {
			return ticket == 72;
		}
		bool Release(uint64_t ticket) override {
			return ticket == 72;
		}
		void Detach() override {
			if (Detached == 0) ++Detached;
		}
	};
}
TEST_CASE(
	"typed compute service uses copied binary values and one shared schema in both VMs",
	"[scripting][compute]"
) {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		engine::ecs::Store store("typed-service");
		auto endpoint = std::make_shared<Endpoint>();
		engine::script::RuntimeLimits limits;
		limits.Compute = [&](uint64_t runtime, uint64_t generation) {
			endpoint->Owner = {std::string(store.Name()), runtime, generation, UINT32_MAX};
			return endpoint;
		};
		auto runtime = engine::script::MakeRuntime(store, language, limits);
		REQUIRE(runtime);
		const std::string source = language == engine::script::Language::Luau ? R"(
local plan=ComputeService:StockGraph('StableCompactU32',65)
assert(ComputeService:DefineGraph('test',plan)=='71')
local input=buffer.create(4); buffer.writeu32(input,0,42)
local ticket=ComputeService:SubmitGraph('test',{Inputs={{Name='input',Type='u32',Count=1}},Outputs={{Name='packed',MaximumElements=65}}},input)
buffer.writeu32(input,0,99)
assert(ticket=='72' and ComputeService:PollGraph(ticket).Outputs[1].Count==1)
assert(buffer.readu32(ComputeService:ReadGraphResult(ticket,'packed',0,4),0)==84)
assert(ComputeService:CancelGraph(ticket) and ComputeService:ReleaseGraph(ticket))
assert(not pcall(function() ComputeService:SubmitGraph('test',{Inputs={{Name='input',Type='u32',Count=2}},Outputs={}},input) end))
assert(not pcall(function() ComputeService:DefineGraph('test',{Buffers={},Kernels={},Passes={},Unexpected=true}) end))
assert(not pcall(function() ComputeService:PollGraph(72) end))
)"
																			  : R"(
const plan=ComputeService.StockGraph('StableCompactU32',65);
function check(value) { if (!value) throw new Error('typed compute service differs'); }
check(ComputeService.DefineGraph('test',plan)==='71');
const input=new ArrayBuffer(4); new DataView(input).setUint32(0,42,true);
const ticket=ComputeService.SubmitGraph('test',{Inputs:[{Name:'input',Type:'u32',Count:1}],Outputs:[{Name:'packed',MaximumElements:65}]},input);
new DataView(input).setUint32(0,99,true);
check(ticket==='72' && ComputeService.PollGraph(ticket).Outputs[0].Count===1);
check(new DataView(ComputeService.ReadGraphResult(ticket,'packed',0,4)).getUint32(0,true)===84);
check(ComputeService.CancelGraph(ticket) && ComputeService.ReleaseGraph(ticket));
function refuses(fn) { try { fn(); return false; } catch (_) { return true; } }
check(refuses(()=>ComputeService.SubmitGraph('test',{Inputs:[{Name:'input',Type:'u32',Count:2}],Outputs:[]},input)));
check(refuses(()=>ComputeService.DefineGraph('test',{Buffers:[],Kernels:[],Passes:[],Unexpected:true})));
check(refuses(()=>ComputeService.PollGraph(72)));
)";
		INFO(runtime->LastError());
		REQUIRE(runtime->Run(source));
		CHECK(endpoint->Owner.Runtime == runtime->Identity());
		CHECK(endpoint->Owner.Generation == store.Identity());
		REQUIRE(endpoint->Defined.Definition.Kernels.size() == 4);
		REQUIRE(endpoint->Submitted.Inputs.size() == 1);
		uint32_t copied = 0;
		std::memcpy(&copied, endpoint->Submitted.Inputs[0].Bytes.data(), 4);
		CHECK(copied == 42);
		store.Clear();
		CHECK_FALSE(runtime->Run(
			language == engine::script::Language::Luau ? "ComputeService:PollGraph('72')"
													   : "ComputeService.PollGraph('72')"
		));
		CHECK(runtime->LastError().find("stale request owner") != std::string::npos);
		runtime.reset();
		CHECK(endpoint->Detached == 1);
	}
}
TEST_CASE(
	"typed compute refuses explicitly on headless runtimes without a device endpoint", "[scripting][compute]"
) {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		engine::ecs::Store store("headless-compute");
		auto runtime = engine::script::MakeRuntime(store, language);
		REQUIRE(runtime);
		const char *source =
			language == engine::script::Language::Luau
				? "ComputeService:DefineGraph('test',ComputeService:StockGraph('StableCompactU32',0))"
				: "ComputeService.DefineGraph('test',ComputeService.StockGraph('StableCompactU32',0))";
		CHECK_FALSE(runtime->Run(source));
		CHECK(runtime->LastError().find("compute device unavailable") != std::string::npos);
	}
}

TEST_CASE("compute metadata is bounded before owned tree allocation in both VMs", "[scripting][compute]") {
	engine::scene::EnsureClassTree();
	engine::scene::RegisterSceneComponents();
	for (const auto language : {engine::script::Language::Luau, engine::script::Language::JavaScript}) {
		CAPTURE(static_cast<int>(language));
		engine::ecs::Store store("bounded-compute");
		auto endpoint = std::make_shared<Endpoint>();
		engine::script::RuntimeLimits limits;
		limits.Compute = [&](uint64_t runtime, uint64_t generation) {
			endpoint->Owner = {std::string(store.Name()), runtime, generation, UINT32_MAX};
			return endpoint;
		};
		auto runtime = engine::script::MakeRuntime(store, language, limits);
		REQUIRE(runtime);
		const char *source = language == engine::script::Language::Luau ? R"(
local function refuses(record, expected)
    local ok,detail=pcall(function() ComputeService:DefineGraph('test',record) end)
    assert(not ok and string.find(tostring(detail),expected,1,true),tostring(detail))
end
refuses({Buffers=table.create(1000000,false),Kernels={},Passes={}},'bounded record')
refuses({Text=string.rep('x',600000)},'bounded record')
refuses({[string.rep('k',600000)]=1},'bounded record')
local many={}; for i=1,6000 do many['field'..i]=i end
refuses({Nested={Child=many}},'bounded record')
refuses({First=string.rep('a',40000),Second=string.rep('b',40000)},'64 KiB')
local plan=ComputeService:StockGraph('StableCompactU32',65)
assert(ComputeService:DefineGraph('test',plan)=='71')
assert(ComputeService:SubmitGraph('test',{Inputs={},Outputs={}})=='72')
)"
																		: R"(
function refuses(record,expected) {
    let detail=''; try { ComputeService.DefineGraph('test',record); } catch(error) { detail=String(error); }
    if (!detail.includes(expected)) throw new Error('metadata refusal differs: '+detail);
}
let touched=false;
const large=new Array(1000000);
Object.defineProperty(large,0,{get() { touched=true; return false; }});
refuses({Buffers:large,Kernels:[],Passes:[]},'bounded record');
if (touched) throw new Error('oversized array was traversed before refusal');
refuses({Text:'x'.repeat(600000)},'bounded record');
let keyTouched=false;
const badKey={};
Object.defineProperty(badKey,'k'.repeat(600000),{enumerable:true,get() { keyTouched=true; return 1; }});
refuses(badKey,'bounded record');
if (keyTouched) throw new Error('oversized key getter ran after refusal');
const many={}; for (let i=0;i<6000;++i) many['field'+i]=i;
refuses({Nested:{Child:many}},'bounded record');
refuses({First:'a'.repeat(40000),Second:'b'.repeat(40000)},'64 KiB');
const plan=ComputeService.StockGraph('StableCompactU32',65);
if (ComputeService.DefineGraph('test',plan)!=='71') throw new Error('valid stock plan refused');
if (ComputeService.SubmitGraph('test',{Inputs:[],Outputs:[]})!=='72') throw new Error('empty request refused');
)";
		const bool ran = runtime->Run(source);
		INFO(runtime->LastError());
		REQUIRE(ran);
		CHECK(endpoint->Definitions == 1);
		CHECK(endpoint->Submitted.Inputs.empty());
		CHECK(endpoint->Submitted.Outputs.empty());
	}
}
