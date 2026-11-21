#include <engine/testing/Suite.hpp>

#include <catch2/catch_test_macros.hpp>

#include <apidump/ApiDump.hpp>
#include <nlohmann/json.hpp>

TEST_SUITE_ID("tools.apidump")
TEST_TIER("fast")

namespace {

	using Json = nlohmann::json;

	constexpr const char *DECLARATIONS = R"(
declare extern type Vector3 with
	X: number
	function Dot(self, other: Vector3): number
	function __add(self, other: Vector3): Vector3
end

declare Vector3: {
	new: (x: number?, y: number?, z: number?) -> Vector3,
	zero: Vector3,
}

declare extern type RBXScriptConnection with
	function Disconnect(self): ()
end

declare extern type ChangedSignal with
	function Connect(self, handler: (property: string) -> ()): RBXScriptConnection
end

declare extern type EnumItem with
	Name: string
end

declare extern type Enum_Material extends EnumItem with
end

declare extern type Instance with
	Name: string
	Changed: ChangedSignal
	function Destroy(self): ()
	function Clone(self): Instance
end

declare extern type Part extends Instance with
	Anchored: boolean
	Material: Enum_Material
end

declare extern type Plugin extends Instance with
	function SaveFile(self, name: string, data: string | buffer): string?
end

type Services = { Workspace: Instance }

declare workspace: Instance
declare plugin: Plugin
declare warn: (...any) -> ()
declare Enum: { Material: { Plastic: Enum_Material } }
)";

	apidump::Reflection SampleReflection() {
		apidump::Reflection reflection;
		reflection.Classes.push_back({"Instance", "", {"NotCreatable"}, {{"Name", "Name", "field", true}}});
		reflection.Classes.push_back(
			{"Part",
			 "Instance",
			 {},
			 {{"Name", "Name", "field", true},
			  {"Anchored", "bool", "field", true},
			  {"Material", "enum", "field", true},
			  {"Size", "Vector3", "computed", true}}}
		);
		reflection.Enums.push_back({"Material", {"Plastic", "Wood"}});
		reflection.Services.push_back({"Workspace", {"world"}, "always", {"luau", "javascript"}});
		reflection.GlobalTags.push_back({"plugin", {"plugin-host"}});
		return reflection;
	}

	const Json &Named(const Json &list, const std::string &name) {
		for (const Json &entry : list) {
			if (entry["name"] == name) {
				return entry;
			}
		}
		static const Json NONE;
		return NONE;
	}

	Json Generate() {
		const apidump::Result result = apidump::Generate(DECLARATIONS, SampleReflection());
		REQUIRE(result.Errors.empty());
		REQUIRE(result.Ok);
		return Json::parse(result.Json);
	}

}

TEST_CASE("A class carries reflected properties and declared methods and events", "[apidump]") {
	const Json dump = Generate();

	const Json &part = Named(dump["classes"], "Part");
	REQUIRE(part.is_object());
	CHECK(part["superclass"] == "Instance");

	const Json &anchored = Named(part["members"], "Anchored");
	CHECK(anchored["kind"] == "property");
	CHECK(anchored["type"] == "boolean");
	CHECK(anchored["valueType"] == "bool");
	CHECK(anchored["storage"] == "field");
	CHECK(Named(part["members"], "Material")["type"] == "Enum.Material");

	// Reflected but undeclared still appears; inherited does not repeat.
	CHECK(Named(part["members"], "Size")["storage"] == "computed");
	CHECK(Named(part["members"], "Name").is_null());

	const Json &instance = Named(dump["classes"], "Instance");
	CHECK(Named(instance["members"], "Destroy")["kind"] == "method");
	CHECK(Named(instance["members"], "Clone")["returns"] == "Instance");
	const Json &changed = Named(instance["members"], "Changed");
	CHECK(changed["kind"] == "event");
	REQUIRE(changed["parameters"].size() == 1);
	CHECK(changed["parameters"][0]["name"] == "property");
	CHECK(changed["parameters"][0]["type"] == "string");
}

TEST_CASE("A declared instance class outside the table is still listed", "[apidump]") {
	const Json dump = Generate();

	const Json &plugin = Named(dump["classes"], "Plugin");
	REQUIRE(plugin.is_object());
	CHECK(plugin["superclass"] == "Instance");
	const Json &save = Named(plugin["members"], "SaveFile");
	REQUIRE(save["parameters"].size() == 2);
	CHECK(save["parameters"][1]["type"] == "buffer | string");
	CHECK(save["returns"] == "string?");
}

TEST_CASE("A datatype carries its constructors, constants and operators", "[apidump]") {
	const Json dump = Generate();

	const Json &vector = Named(dump["datatypes"], "Vector3");
	REQUIRE(vector.is_object());
	CHECK(Named(vector["members"], "X")["type"] == "number");
	CHECK(Named(vector["members"], "Dot")["parameters"][0]["name"] == "other");

	const Json &constructor = Named(vector["constructors"], "new");
	REQUIRE(constructor["parameters"].size() == 3);
	CHECK(constructor["parameters"][0]["type"] == "number?");
	CHECK(constructor["returns"] == "Vector3");
	CHECK(Named(vector["constants"], "zero")["type"] == "Vector3");
	CHECK(Named(vector["operators"], "__add")["returns"] == "Vector3");

	// Enum types live under `enums`, never as datatypes.
	CHECK(Named(dump["datatypes"], "Enum_Material").is_null());
	CHECK(Named(dump["datatypes"], "EnumItem").is_null());
}

TEST_CASE("Enums, services and globals come out typed and tagged", "[apidump]") {
	const Json dump = Generate();

	const Json &material = Named(dump["enums"], "Material");
	REQUIRE(material["items"].size() == 2);
	CHECK(material["items"][1]["name"] == "Wood");
	CHECK(material["items"][1]["value"] == 1);

	const Json &workspace = Named(dump["services"], "Workspace");
	CHECK(workspace["capabilities"] == Json::array({"world"}));

	CHECK(Named(dump["globals"], "Enum")["kind"] == "enums");
	CHECK(Named(dump["globals"], "Vector3")["datatype"] == "Vector3");
	CHECK(Named(dump["globals"], "workspace")["type"] == "Instance");
	CHECK(Named(dump["globals"], "plugin")["tags"] == Json::array({"plugin-host"}));

	const Json &warn = Named(dump["globals"], "warn");
	CHECK(warn["kind"] == "function");
	CHECK(warn["overloads"][0]["parameters"][0]["name"] == "...");

	// Luau's own globals are not engine surface.
	CHECK(Named(dump["globals"], "print").is_null());
	CHECK(Named(dump["globals"], "math").is_null());
}

TEST_CASE("Generation is deterministic", "[apidump]") {
	const apidump::Result first = apidump::Generate(DECLARATIONS, SampleReflection());
	const apidump::Result second = apidump::Generate(DECLARATIONS, SampleReflection());
	REQUIRE(first.Ok);
	CHECK(first.Json == second.Json);
}

TEST_CASE("Declarations that do not load report why", "[apidump]") {
	const apidump::Result result = apidump::Generate("declare extern type Broken with", {});
	CHECK_FALSE(result.Ok);
	CHECK_FALSE(result.Errors.empty());
}
