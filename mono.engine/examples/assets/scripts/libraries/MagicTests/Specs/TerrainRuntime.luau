--!strict

-- The runtime owns visible Parts, so this is deliberately separate from the
-- TerrainCore parity tests. It checks the publication boundary that prevents a
-- crater from appearing as a sequence of chunk-sized patches.

return function(harness: any, _core: any, _fixtures: any, terrain: any, runtime: any)
	if terrain == nil or runtime == nil then
		return
	end

	harness:suite("terrain runtime", function(h: any)
		h:test("publishes an edited multi-chunk crater only after every chunk is prepared", function()
			local root = Instance.new("Folder")
			root.Name = "TerrainRuntimeTest"
			root.Parent = workspace

			local edits = terrain.Edits.new()
			local field = terrain.HeightField.new({ seed = 1234 }, edits)
			local builder = runtime.New(field, 16, root)
			builder:build(0, 0)
			builder:build(1, 0)

			local before = builder:stats().publication
			local queued = builder:applyEdit(edits:crater(16, 8, 9, 4, 1))
			h:assertTrue(queued >= 2, "crater did not reach both loaded chunks")

			builder:pump(1)
			h:assertEqual(builder:stats().publication, before)

			while builder:queued() > 0 do
				builder:pump(1)
			end
			h:assertEqual(builder:stats().publication, before + 1)

			root:Destroy()
		end)
	end)
end
