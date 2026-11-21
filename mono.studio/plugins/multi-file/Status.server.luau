--!strict
-- A second script under the same plugin. It runs after `main.luau`, in the
-- same runtime, and shares its module cache.
plugin.Unloading:Connect(function()
	print("multi-file unloading")
end)
