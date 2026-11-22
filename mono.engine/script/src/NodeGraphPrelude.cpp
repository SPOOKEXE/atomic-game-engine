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

local function changed(graph)
	for view in graph._views do
		view:Refresh()
	end
end

function Graph.new(registry)
	return setmetatable({ _handle = Service:CreateGraph(registry._handle), Registry = registry, _views = {} }, Graph)
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
	Service:Destroy(self._handle)
end

local HOOKS = { "PreRun", "PostRun", "PreNode", "PostNode", "PreCompute", "PostCompute" }

local Runner = {}
Runner.__index = Runner

function Runner.new(graph, domain)
	local runner = setmetatable({ _handle = Service:CreateRunner(graph._handle, domain), Graph = graph }, Runner)
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
	return report
end

-- Script nodes are Sync in every domain, so nothing a script can define
-- waits across calls and this answers at once.
function Runner:RunAsync(inputs)
	return self:Run(inputs)
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

	class Registry {
		constructor() {
			this._handle = Service.CreateRegistry();
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
		constructor(registry) {
			this._handle = Service.CreateGraph(registry._handle);
			this.Registry = registry;
			this._views = new Set();
		}
		_changed() {
			for (const view of this._views) view.Refresh();
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
			Service.Destroy(this._handle);
		}
	}

	const HOOKS = ["PreRun", "PostRun", "PreNode", "PostNode", "PreCompute", "PostCompute"];

	class Runner {
		constructor(graph, domain) {
			this._handle = Service.CreateRunner(graph._handle, domain);
			this.Graph = graph;
			for (const name of HOOKS) this[name] = makeSignal(this, name);
		}
		Run(inputs) {
			for (const view of this.Graph._views) Service.SyncView(view._handle);
			const report = Service.Run(this._handle, inputs);
			for (const view of this.Graph._views) {
				Service.SetViewRun(view._handle, this._handle);
				view.Refresh();
			}
			return report;
		}
		// Script nodes are Sync in every domain, so this settles at once.
		RunAsync(inputs) {
			try {
				return Promise.resolve(this.Run(inputs));
			} catch (error) {
				return Promise.reject(error);
			}
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
