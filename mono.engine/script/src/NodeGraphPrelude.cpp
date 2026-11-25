// The `NodeGraph` objects scripts use, built over `NodeGraphService` handles.
//
// One source per language, run once per VM with the service as its argument.
// They hold no state the service does not: a script object is a handle plus
// the signal tables for its hooks, so both languages answer every question
// through the same neutral rows.

#include <engine/script/NodeGraphService.hpp>

namespace engine::script {

	std::string_view NodeGraphLuauPrelude() {
		return R"luau(
local Service = ...

local function makeSignal(runner, name)
	local handlers = {}
	local armed = false
	local function dispatch(info)
		for _, handler in table.clone(handlers) do
			handler(info)
		end
	end
	local signal = {}
	function signal:Connect(handler)
		if type(handler) ~= "function" then
			error("Connect expects a function", 2)
		end
		if not armed then
			Service:SetHook(runner._handle, name, dispatch)
			armed = true
		end
		table.insert(handlers, handler)
		local connection = { Connected = true }
		function connection:Disconnect()
			if not connection.Connected then
				return
			end
			connection.Connected = false
			local at = table.find(handlers, handler)
			if at then
				table.remove(handlers, at)
			end
			if #handlers == 0 and armed then
				Service:SetHook(runner._handle, name, nil)
				armed = false
			end
		end
		return connection
	end
	return table.freeze(signal)
end

local Registry = {}
Registry.__index = Registry

function Registry.new()
	return setmetatable({ _handle = Service:CreateRegistry() }, Registry)
end

function Registry:DefineType(id, spec)
	Service:DefineType(self._handle, id, spec)
end

function Registry:DefineNode(spec)
	local copy = table.clone(spec)
	local evaluate = copy.Evaluate
	copy.Evaluate = nil
	Service:DefineNode(self._handle, copy, evaluate)
end

function Registry:Destroy()
	Service:Destroy(self._handle)
end

local Graph = {}
Graph.__index = Graph

local ATTRIBUTE = "NodeGraphDocument"

-- Writes the document to the instance it is published on, when it changed.
local function publish(graph)
	local target = graph._published
	if target ~= nil then
		local text = Service:Save(graph._handle)
		if text ~= graph._text then
			graph._text = text
			target:SetAttribute(ATTRIBUTE, text)
		end
	end
end

local function changed(graph)
	for view in graph._views do
		view:Refresh()
	end
	publish(graph)
end

local function wrap(handle, registry)
	return setmetatable({ _handle = handle, Registry = registry, _views = {} }, Graph)
end

local function opened(handles)
	return wrap(handles.Graph, setmetatable({ _handle = handles.Registry }, Registry))
end

function Graph.new(registry)
	return wrap(Service:CreateGraph(registry._handle), registry)
end

function Graph.load(registry, text)
	local graph = Graph.new(registry)
	graph:Load(text)
	return graph
end

-- The engine's own graphs. An image graph can be edited and committed; the
-- others are read-only.
function Graph.fromImageGraph(instance)
	local graph = opened(Service:OpenImageGraph(instance:GetGraphDocument()))
	graph._image = instance
	return graph
end

function Graph.fromProcedural()
	return opened(Service:OpenProcedural())
end

function Graph.hostNames()
	return Service:HostGraphNames()
end

function Graph.fromHost(name)
	return opened(Service:OpenHostGraph(name))
end

-- A graph that follows the document published on an instance, such as one a
-- server published and this client received.
function Graph.subscribe(registry, instance)
	local graph = Graph.new(registry)
	local function reload()
		local text = instance:GetAttribute(ATTRIBUTE)
		if text ~= nil then
			local ok, message = pcall(graph.Load, graph, text)
			if not ok then
				warn("NodeGraph: published graph refused: " .. tostring(message))
			end
		end
	end
	reload()
	graph._connection = instance:GetAttributeChangedSignal(ATTRIBUTE):Connect(reload)
	return graph
end

function Graph:PublishTo(instance)
	self._published = instance
	self._text = nil
	publish(self)
end

function Graph:Save()
	return Service:Save(self._handle)
end

function Graph:Load(text)
	Service:Load(self._handle, text)
	-- Views drop what they last drew, so a canvas does not write it back.
	for view in self._views do
		Service:ResetView(view._handle)
	end
	changed(self)
end

function Graph:Commit()
	for view in self._views do
		Service:SyncView(view._handle)
	end
	local document = Service:WriteImageGraph(self._handle)
	self._image:SetGraphDocument(document)
end

function Graph:AddNode(def, params, position, id)
	local x, y = nil, nil
	if position ~= nil then
		x, y = position.X, position.Y
	end
	local made = Service:AddNode(self._handle, def, params, x, y, id)
	changed(self)
	return made
end

function Graph:RemoveNode(node)
	local removed = Service:RemoveNode(self._handle, node)
	changed(self)
	return removed
end

function Graph:Link(from, fromPort, to, toPort)
	Service:Link(self._handle, from, fromPort, to, toPort)
	changed(self)
end

function Graph:Unlink(from, fromPort, to, toPort)
	local removed = Service:Unlink(self._handle, from, fromPort, to, toPort)
	changed(self)
	return removed
end

function Graph:SetParam(node, name, value)
	Service:SetParam(self._handle, node, name, value)
	changed(self)
end

function Graph:MoveNode(node, position)
	Service:MoveNode(self._handle, node, position.X, position.Y)
	changed(self)
end

function Graph:SetBypass(node, bypassed)
	Service:SetBypass(self._handle, node, bypassed)
	changed(self)
end

function Graph:SetCollapsed(node, collapsed)
	Service:SetCollapsed(self._handle, node, collapsed)
	changed(self)
end

function Graph:GetNodes()
	return Service:GetNodes(self._handle)
end

function Graph:GetLinks()
	return Service:GetLinks(self._handle)
end

function Graph:Validate(domain)
	return Service:Validate(self._handle, domain)
end

function Graph:Destroy()
	if self._connection ~= nil then
		self._connection:Disconnect()
		self._connection = nil
	end
	self._published = nil
	Service:Destroy(self._handle)
end

local HOOKS = { "PreRun", "PostRun", "PreNode", "PostNode", "PreCompute", "PostCompute" }

local Runner = {}
Runner.__index = Runner

function Runner.new(graph, domain)
	local runner = setmetatable({
		_handle = Service:CreateRunner(graph._handle, domain),
		Graph = graph,
		Domain = domain or "Tool",
	}, Runner)
	for _, name in HOOKS do
		runner[name] = makeSignal(runner, name)
	end
	return runner
end

function Runner:Run(inputs)
	local graph = self.Graph
	for view in graph._views do
		Service:SyncView(view._handle)
	end
	local report = Service:Run(self._handle, inputs)
	for view in graph._views do
		Service:SetViewRun(view._handle, self._handle)
		view:Refresh()
	end
	publish(graph)
	return report
end

-- Runs until no node waits on async work, yielding a tick between passes.
-- Impure nodes run again on each pass.
function Runner:RunAsync(inputs)
	if self.Domain == "Tick" then
		error("RunAsync is for Frame and Tool graphs", 2)
	end
	local report = self:Run(inputs)
	while not report.Complete do
		task.wait()
		report = self:Run(inputs)
	end
	return report
end

function Runner:Output(node, port)
	return Service:Output(self._handle, node, port)
end

function Runner:Destroy()
	Service:Destroy(self._handle)
end

local View = {}

local function viewIndex(view, key)
	if key == "Parent" then
		return rawget(view, "_parent")
	elseif key == "Paint" then
		return rawget(view, "_paint")
	end
	return View[key]
end

local function viewNewIndex(view, key, value)
	if key == "Parent" then
		rawset(view, "_parent", value)
		view:Refresh()
	elseif key == "Paint" then
		rawset(view, "_paint", value)
		view:Refresh()
	elseif key == "Size" then
		rawset(view, "_size", value)
	else
		error("NodeGraph.View has no settable member " .. tostring(key), 2)
	end
end

function View.new(graph, options)
	local view = setmetatable({ _handle = Service:CreateView(graph._handle, options), Graph = graph }, {
		__index = viewIndex,
		__newindex = viewNewIndex,
	})
	graph._views[view] = true
	return view
end

function View:SetTheme(options)
	Service:SetViewTheme(self._handle, options)
	self:Refresh()
end

function View:GetDrawCommands(size)
	local area = size or rawget(self, "_size") or Vector2.new(800, 600)
	return Service:GetDrawCommands(self._handle, area.X, area.Y)
end

function View:Refresh()
	local paint = rawget(self, "_paint")
	if paint ~= nil then
		paint(self:GetDrawCommands(), self.Graph)
		return
	end
	local parent = rawget(self, "_parent")
	if parent ~= nil then
		Service:RenderView(self._handle, parent)
	end
end

function View:Destroy()
	self.Graph._views[self] = nil
	Service:Destroy(self._handle)
end

return table.freeze({
	Registry = table.freeze(Registry),
	Graph = table.freeze(Graph),
	Runner = table.freeze(Runner),
	View = table.freeze(View),
	Themes = Service:ThemeNames(),
	Domains = Service:Domains(),
})
)luau";
	}

	std::string_view NodeGraphJavaScriptPrelude() {
		return R"js(
(function (Service) {
	"use strict";

	function makeSignal(runner, name) {
		const handlers = [];
		let armed = false;
		const dispatch = (info) => {
			for (const handler of handlers.slice()) handler(info);
		};
		return Object.freeze({
			Connect(handler) {
				if (typeof handler !== "function") throw new TypeError("Connect expects a function");
				if (!armed) {
					Service.SetHook(runner._handle, name, dispatch);
					armed = true;
				}
				handlers.push(handler);
				const connection = {
					Connected: true,
					Disconnect() {
						if (!connection.Connected) return;
						connection.Connected = false;
						const at = handlers.indexOf(handler);
						if (at >= 0) handlers.splice(at, 1);
						if (handlers.length === 0 && armed) {
							Service.SetHook(runner._handle, name, undefined);
							armed = false;
						}
					},
				};
				return connection;
			},
		});
	}

	const ATTRIBUTE = "NodeGraphDocument";

	class Registry {
		constructor(handle) {
			this._handle = handle === undefined ? Service.CreateRegistry() : handle;
		}
		DefineType(id, spec) {
			Service.DefineType(this._handle, id, spec);
		}
		DefineNode(spec) {
			const copy = Object.assign({}, spec);
			const evaluate = copy.Evaluate;
			delete copy.Evaluate;
			Service.DefineNode(this._handle, copy, evaluate);
		}
		Destroy() {
			Service.Destroy(this._handle);
		}
	}

	class Graph {
		constructor(registry, handle) {
			this._handle = handle === undefined ? Service.CreateGraph(registry._handle) : handle;
			this.Registry = registry;
			this._views = new Set();
		}
		static _opened(handles) {
			return new Graph(new Registry(handles.Registry), handles.Graph);
		}
		static load(registry, text) {
			const graph = new Graph(registry);
			graph.Load(text);
			return graph;
		}
		// The engine's own graphs. An image graph can be edited and committed;
		// the others are read-only.
		static fromImageGraph(instance) {
			const graph = Graph._opened(Service.OpenImageGraph(instance.GetGraphDocument()));
			graph._image = instance;
			return graph;
		}
		static fromProcedural() {
			return Graph._opened(Service.OpenProcedural());
		}
		static hostNames() {
			return Service.HostGraphNames();
		}
		static fromHost(name) {
			return Graph._opened(Service.OpenHostGraph(name));
		}
		// A graph that follows the document published on an instance, such as
		// one a server published and this client received.
		static subscribe(registry, instance) {
			const graph = new Graph(registry);
			const reload = () => {
				const text = instance.GetAttribute(ATTRIBUTE);
				if (text === undefined || text === null) return;
				try {
					graph.Load(text);
				} catch (error) {
					console.warn("NodeGraph: published graph refused: " + error);
				}
			};
			reload();
			graph._connection = instance.GetAttributeChangedSignal(ATTRIBUTE).Connect(reload);
			return graph;
		}
		// Writes the document to the instance it is published on, when it changed.
		_publish() {
			if (!this._published) return;
			const text = Service.Save(this._handle);
			if (text !== this._text) {
				this._text = text;
				this._published.SetAttribute(ATTRIBUTE, text);
			}
		}
		_changed() {
			for (const view of this._views) view.Refresh();
			this._publish();
		}
		PublishTo(instance) {
			this._published = instance;
			this._text = undefined;
			this._publish();
		}
		Save() {
			return Service.Save(this._handle);
		}
		Load(text) {
			Service.Load(this._handle, text);
			// Views drop what they last drew, so a canvas does not write it back.
			for (const view of this._views) Service.ResetView(view._handle);
			this._changed();
		}
		Commit() {
			for (const view of this._views) Service.SyncView(view._handle);
			this._image.SetGraphDocument(Service.WriteImageGraph(this._handle));
		}
		SetCollapsed(node, collapsed) {
			Service.SetCollapsed(this._handle, node, collapsed);
			this._changed();
		}
		AddNode(def, params, position, id) {
			const made = Service.AddNode(this._handle, def, params, position ? position.X : undefined, position ? position.Y : undefined, id);
			this._changed();
			return made;
		}
		RemoveNode(node) {
			const removed = Service.RemoveNode(this._handle, node);
			this._changed();
			return removed;
		}
		Link(from, fromPort, to, toPort) {
			Service.Link(this._handle, from, fromPort, to, toPort);
			this._changed();
		}
		Unlink(from, fromPort, to, toPort) {
			const removed = Service.Unlink(this._handle, from, fromPort, to, toPort);
			this._changed();
			return removed;
		}
		SetParam(node, name, value) {
			Service.SetParam(this._handle, node, name, value);
			this._changed();
		}
		MoveNode(node, position) {
			Service.MoveNode(this._handle, node, position.X, position.Y);
			this._changed();
		}
		SetBypass(node, bypassed) {
			Service.SetBypass(this._handle, node, bypassed);
			this._changed();
		}
		GetNodes() {
			return Service.GetNodes(this._handle);
		}
		GetLinks() {
			return Service.GetLinks(this._handle);
		}
		Validate(domain) {
			return Service.Validate(this._handle, domain);
		}
		Destroy() {
			if (this._connection) this._connection.Disconnect();
			this._connection = undefined;
			this._published = undefined;
			Service.Destroy(this._handle);
		}
	}

	const HOOKS = ["PreRun", "PostRun", "PreNode", "PostNode", "PreCompute", "PostCompute"];

	class Runner {
		constructor(graph, domain) {
			this._handle = Service.CreateRunner(graph._handle, domain);
			this.Graph = graph;
			this.Domain = domain === undefined ? "Tool" : domain;
			for (const name of HOOKS) this[name] = makeSignal(this, name);
		}
		Run(inputs) {
			for (const view of this.Graph._views) Service.SyncView(view._handle);
			const report = Service.Run(this._handle, inputs);
			for (const view of this.Graph._views) {
				Service.SetViewRun(view._handle, this._handle);
				view.Refresh();
			}
			this.Graph._publish();
			return report;
		}
		// Runs until no node waits on async work, a tick between passes. Impure
		// nodes run again on each pass.
		async RunAsync(inputs) {
			if (this.Domain === "Tick") throw new Error("RunAsync is for Frame and Tool graphs");
			let report = this.Run(inputs);
			while (!report.Complete) {
				await task.wait(0);
				report = this.Run(inputs);
			}
			return report;
		}
		Output(node, port) {
			return Service.Output(this._handle, node, port);
		}
		Destroy() {
			Service.Destroy(this._handle);
		}
	}

	class View {
		constructor(graph, options) {
			this._handle = Service.CreateView(graph._handle, options);
			this.Graph = graph;
			this._parent = undefined;
			this._paint = undefined;
			this.Size = undefined;
			graph._views.add(this);
		}
		get Parent() {
			return this._parent;
		}
		set Parent(value) {
			this._parent = value;
			this.Refresh();
		}
		get Paint() {
			return this._paint;
		}
		set Paint(value) {
			this._paint = value;
			this.Refresh();
		}
		SetTheme(options) {
			Service.SetViewTheme(this._handle, options);
			this.Refresh();
		}
		GetDrawCommands(size) {
			const area = size || this.Size || { X: 800, Y: 600 };
			return Service.GetDrawCommands(this._handle, area.X, area.Y);
		}
		Refresh() {
			if (this._paint) {
				this._paint(this.GetDrawCommands(), this.Graph);
				return;
			}
			if (this._parent) Service.RenderView(this._handle, this._parent);
		}
		Destroy() {
			this.Graph._views.delete(this);
			Service.Destroy(this._handle);
		}
	}

	return Object.freeze({
		Registry,
		Graph,
		Runner,
		View,
		Themes: Service.ThemeNames(),
		Domains: Service.Domains(),
	});
})
)js";
	}
}
