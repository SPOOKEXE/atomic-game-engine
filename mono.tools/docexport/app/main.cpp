// Standalone HTML documentation exporter.
//
// Merges all HTML files from the documentation build directory into a single
// self-contained HTML file with interactive three.js architecture diagrams.

#include <engine/core/Arguments.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

	struct DocPage {
		std::string path;
		std::string title;
		std::string body;
	};

	std::string ReadFile(const std::filesystem::path &path) {
		std::ifstream file(path, std::ios::binary);
		if (!file) return {};
		return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	}

	std::string ExtractBody(const std::string &html) {
		auto body_start = html.find("<body");
		if (body_start == std::string::npos) body_start = html.find("<BODY");
		if (body_start == std::string::npos) return html;

		auto tag_end = html.find('>', body_start);
		if (tag_end == std::string::npos) return html;

		auto body_end = html.rfind("</body");
		if (body_end == std::string::npos) body_end = html.rfind("</BODY");
		if (body_end == std::string::npos) body_end = html.size();

		return html.substr(tag_end + 1, body_end - tag_end - 1);
	}

	std::string ExtractTitle(const std::string &html) {
		auto title_start = html.find("<title>");
		auto title_end = html.find("</title>");
		if (title_start != std::string::npos && title_end != std::string::npos) {
			return html.substr(title_start + 7, title_end - title_start - 7);
		}
		return "Untitled";
	}

	std::vector<DocPage> LoadPages(const std::filesystem::path &docs_dir) {
		std::vector<DocPage> pages;

		if (!std::filesystem::exists(docs_dir)) {
			std::cerr << "docexport: " << docs_dir << " does not exist\n";
			return pages;
		}

		for (const auto &entry : std::filesystem::recursive_directory_iterator(docs_dir)) {
			if (!entry.is_regular_file()) continue;
			if (entry.path().extension() != ".html") continue;

			std::string html = ReadFile(entry.path());
			if (html.empty()) continue;

			DocPage page;
			page.path = std::filesystem::relative(entry.path(), docs_dir).string();
			page.title = ExtractTitle(html);
			page.body = ExtractBody(html);
			pages.push_back(std::move(page));
		}

		std::sort(pages.begin(), pages.end(), [](const auto &a, const auto &b) { return a.path < b.path; });

		return pages;
	}

	std::string EscapeForJs(const std::string &s) {
		std::string result;
		result.reserve(s.size());
		for (char c : s) {
			switch (c) {
			case '\\':
				result += "\\\\";
				break;
			case '\'':
				result += "\\'";
				break;
			case '\n':
				result += "\\n";
				break;
			case '\r':
				result += "\\r";
				break;
			default:
				result += c;
			}
		}
		return result;
	}

	std::string GeneratePageScript(const std::vector<DocPage> &pages) {
		std::ostringstream js;
		js << "var pageData = [\n";
		for (size_t i = 0; i < pages.size(); ++i) {
			js << "  {id:'page" << i << "',title:'" << EscapeForJs(pages[i].title) << "',body:'"
			   << EscapeForJs(pages[i].body) << "'}";
			if (i + 1 < pages.size()) js << ",";
			js << "\n";
		}
		js << "];\n";
		return js.str();
	}

	std::string GenerateHtml(const std::vector<DocPage> &pages) {
		std::ostringstream html;

		html << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
		html << "<meta charset=\"UTF-8\">\n";
		html << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
		html << "<title>Atomic Game Engine - Documentation</title>\n";
		html << "<style>\n";
		html << "* { margin: 0; padding: 0; box-sizing: border-box; }\n";
		html << "body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; "
				"background: #1a1a2e; color: #eee; line-height: 1.6; }\n";
		html << "#header { position: fixed; top: 0; left: 0; right: 0; height: 50px; background: rgba(26, "
				"26, 46, 0.98); border-bottom: 1px solid #333; display: flex; align-items: center; padding: "
				"0 20px; z-index: 100; }\n";
		html << "#header h1 { font-size: 16px; font-weight: 500; }\n";
		html << "#sidebar { position: fixed; top: 50px; left: 0; width: 250px; bottom: 0; background: "
				"rgba(26, 26, 46, 0.95); border-right: 1px solid #333; overflow-y: auto; padding: 10px; }\n";
		html << "#sidebar a { display: block; padding: 6px 10px; color: #aaa; text-decoration: none; "
				"font-size: 13px; border-radius: 4px; }\n";
		html << "#sidebar a:hover, #sidebar a.active { background: rgba(255,255,255,0.1); color: #fff; }\n";
		html << "#content { position: fixed; top: 50px; left: 250px; right: 0; bottom: 0; overflow-y: auto; "
				"padding: 20px 40px; }\n";
		html << "#architecture-view { position: fixed; bottom: 20px; right: 20px; width: 400px; height: "
				"300px; background: rgba(0,0,0,0.3); border-radius: 8px; overflow: hidden; border: 1px solid "
				"#333; }\n";
		html << ".page { display: none; }\n";
		html << ".page.active { display: block; }\n";
		html << ".page h1, .page h2, .page h3 { color: #4CAF50; margin: 20px 0 10px; }\n";
		html << ".page p { margin: 10px 0; color: #ccc; }\n";
		html << ".page code { background: rgba(255,255,255,0.1); padding: 2px 6px; border-radius: 3px; "
				"font-size: 14px; }\n";
		html << ".page pre { background: rgba(0,0,0,0.3); padding: 15px; border-radius: 5px; overflow-x: "
				"auto; margin: 10px 0; }\n";
		html << ".page a { color: #4CAF50; }\n";
		html << ".page ul, .page ol { margin: 10px 0 10px 20px; }\n";
		html << ".page li { margin: 5px 0; color: #ccc; }\n";
		html << ".page table { border-collapse: collapse; margin: 10px 0; width: 100%; }\n";
		html << ".page th, .page td { border: 1px solid #333; padding: 8px 12px; text-align: left; }\n";
		html << ".page th { background: rgba(76, 175, 80, 0.2); }\n";
		html << "</style>\n</head>\n<body>\n";

		html << "<div id=\"header\"><h1>Atomic Game Engine Documentation</h1></div>\n";

		// Sidebar
		html << "<div id=\"sidebar\">\n";
		html << "  <a href=\"#\" onclick=\"showPage('arch')\" "
				"style=\"font-weight:bold;color:#4CAF50;\">Architecture</a>\n";
		for (size_t i = 0; i < pages.size(); ++i) {
			html << "  <a href=\"#\" onclick=\"showPage('page" << i << "')\">" << pages[i].title << "</a>\n";
		}
		html << "</div>\n";

		// Content
		html << "<div id=\"content\">\n";
		html << "  <div id=\"arch\" class=\"page active\">\n";
		html << "    <h1>Architecture Overview</h1>\n";
		html << "    <p>Interactive 3D view of the module hierarchy. Drag to rotate, scroll to zoom.</p>\n";
		html << "    <div id=\"architecture-view\"></div>\n";
		html << "  </div>\n";

		for (size_t i = 0; i < pages.size(); ++i) {
			html << "  <div id=\"page" << i << "\" class=\"page\">" << pages[i].body << "</div>\n";
		}
		html << "</div>\n";

		// Scripts
		html << "<script "
				"src=\"https://cdnjs.cloudflare.com/ajax/libs/three.js/r128/three.min.js\"></script>\n";
		html << "<script>\n";

		// Page data
		html << GeneratePageScript(pages);

		// Navigation
		html << "function showPage(id) {\n";
		html << "  document.querySelectorAll('.page').forEach(function(p) { p.classList.remove('active'); "
				"});\n";
		html << "  document.querySelectorAll('#sidebar a').forEach(function(a) { "
				"a.classList.remove('active'); });\n";
		html << "  var page = document.getElementById(id);\n";
		html << "  if (page) page.classList.add('active');\n";
		html << "  var links = document.querySelectorAll('#sidebar a');\n";
		html << "  links.forEach(function(a) { if (a.getAttribute('onclick') && "
				"a.getAttribute('onclick').indexOf(id) >= 0) a.classList.add('active'); });\n";
		html << "  document.getElementById('content').scrollTop = 0;\n";
		html << "}\n";

		// Three.js
		html << "var scene = new THREE.Scene();\n";
		html << "var camera = new THREE.PerspectiveCamera(75, 400/300, 0.1, 1000);\n";
		html << "var renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });\n";
		html << "renderer.setClearColor(0x1a1a2e, 1);\n";
		html << "renderer.setSize(400, 300);\n";
		html << "var container = document.getElementById('architecture-view');\n";
		html << "if (container) container.appendChild(renderer.domElement);\n";
		html << "camera.position.set(0, 12, 20);\n";
		html << "camera.lookAt(0, 0, 0);\n";
		html << "scene.add(new THREE.AmbientLight(0x404040));\n";
		html << "var dl = new THREE.DirectionalLight(0xffffff, 0.8);\n";
		html << "dl.position.set(10, 20, 10);\n";
		html << "scene.add(dl);\n";
		html << "var tierColors = [0x4CAF50, 0x2196F3, 0x9C27B0, 0xFF9800, 0xF44336, 0x00BCD4, 0x795548, "
				"0x607D8B];\n";
		html << "var modules = [\n";
		html << "  {name:'core',tier:0},{name:'parallel',tier:1},{name:'ecs',tier:2},\n";
		html << "  {name:'world',tier:3},{name:'collision',tier:4},{name:'spatial',tier:4},\n";
		html << "  {name:'scene',tier:5},{name:'gui',tier:5},{name:'assets',tier:6},\n";
		html << "  {name:'physics',tier:6},{name:'effects',tier:6},{name:'script',tier:7},\n";
		html << "  {name:'graph',tier:7},{name:'replication',tier:8},{name:'render',tier:8}\n";
		html << "];\n";
		html << "var moduleMap = {};\n";
		html << "modules.forEach(function(mod, i) {\n";
		html << "  var color = tierColors[mod.tier % tierColors.length];\n";
		html << "  var geometry = new THREE.BoxGeometry(1.5, 0.8, 0.8);\n";
		html << "  var material = new THREE.MeshPhongMaterial({color: color, transparent: true, opacity: "
				"0.85});\n";
		html << "  var cube = new THREE.Mesh(geometry, material);\n";
		html << "  var x = (i - modules.length / 2) * 2;\n";
		html << "  var y = mod.tier * 1.5;\n";
		html << "  cube.position.set(x, y, 0);\n";
		html << "  cube.userData = mod;\n";
		html << "  scene.add(cube);\n";
		html << "  moduleMap[mod.name] = cube;\n";
		html << "});\n";
		html << "var deps = "
				"[['world','ecs'],['scene','world'],['gui','ecs'],['physics','ecs'],['effects','ecs'],['"
				"script','ecs'],['replication','ecs'],['render','gui']];\n";
		html << "deps.forEach(function(d) {\n";
		html << "  if (moduleMap[d[0]] && moduleMap[d[1]]) {\n";
		html << "    var points = [moduleMap[d[0]].position.clone(), moduleMap[d[1]].position.clone()];\n";
		html << "    var geometry = new THREE.BufferGeometry().setFromPoints(points);\n";
		html << "    var material = new THREE.LineBasicMaterial({color: 0x888888, transparent: true, "
				"opacity: 0.5});\n";
		html << "    scene.add(new THREE.Line(geometry, material));\n";
		html << "  }\n";
		html << "});\n";
		html << "var isDragging = false, prevMouse = {x:0, y:0};\n";
		html << "renderer.domElement.addEventListener('mousedown', function(e) { isDragging = true; "
				"prevMouse = {x:e.clientX, y:e.clientY}; });\n";
		html << "renderer.domElement.addEventListener('mousemove', function(e) {\n";
		html << "  if (!isDragging) return;\n";
		html << "  scene.rotation.y += (e.clientX - prevMouse.x) * 0.005;\n";
		html << "  prevMouse = {x:e.clientX, y:e.clientY};\n";
		html << "});\n";
		html << "renderer.domElement.addEventListener('mouseup', function() { isDragging = false; });\n";
		html << "renderer.domElement.addEventListener('mouseleave', function() { isDragging = false; });\n";
		html << "renderer.domElement.addEventListener('wheel', function(e) {\n";
		html << "  e.preventDefault();\n";
		html << "  camera.position.z = Math.max(5, Math.min(40, camera.position.z + e.deltaY * 0.02));\n";
		html << "}, false);\n";
		html << "function animate() { requestAnimationFrame(animate); renderer.render(scene, camera); }\n";
		html << "animate();\n";

		html << "</script>\n</body>\n</html>\n";

		return html.str();
	}

} // namespace

int main(int argc, char **argv) {
	engine::core::Arguments arguments(
		"docexport", "atomic - exports standalone HTML documentation with 3D architecture diagrams."
	);
	arguments.Value("docs-dir", "DIR", "Path to built HTML documentation directory");
	arguments.Value("out", "FILE", "Output HTML file (default: docs/standalone.html)");

	const engine::core::Arguments::Result parsed = arguments.Parse(argc, argv);
	if (!parsed.Ok) {
		std::cerr << parsed.Error << "\n\n" << arguments.Help();
		return 2;
	}
	if (parsed.VersionRequested) {
		std::cout << arguments.VersionLine();
		return 0;
	}
	if (parsed.HelpRequested) {
		std::cout << arguments.Help();
		return 0;
	}
	if (parsed.DescribeRequested) {
		std::cout << arguments.Describe();
		return 0;
	}

	std::string docs_dir{arguments.Get("docs-dir").value_or(".cache/build/dev/docs/html")};
	std::string out_path{arguments.Get("out").value_or("docs/standalone.html")};

	auto pages = LoadPages(docs_dir);
	if (pages.empty()) {
		std::cerr << "docexport: no HTML pages found in " << docs_dir << "\n";
		return 1;
	}

	std::cout << "docexport: loaded " << pages.size() << " pages\n";

	std::string html = GenerateHtml(pages);

	std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
	if (!out) {
		std::cerr << "docexport: cannot write " << out_path << "\n";
		return 1;
	}
	out << html;
	out.close();

	std::cout << "docexport: wrote " << out_path << " (" << html.size() << " bytes)\n";
	return 0;
}
