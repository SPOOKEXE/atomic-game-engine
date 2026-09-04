--!strict
-- A deliberately small declarative instance builder for authored demo interfaces.
--
-- This is the part of Fusion's shape this engine can execute today: construct an
-- instance, write its properties, parent its children and connect its events.
-- Reactive observers belong in the engine's eventual UI runtime rather than in
-- a demo-local scheduler that would need a second answer for invalidation.

local Fusion = {}

export type Properties = { [string]: any }

Fusion.Children = "__fusion_children"

function Fusion.OnEvent(name: string): string
	return "__fusion_event_" .. name
end

function Fusion.New(className: string): (Properties) -> Instance
	return function(properties: Properties): Instance
		local create = Instance.new :: any
		local object = create(className) :: Instance
		local writable = object :: any

		for key, value in properties do
			if key ~= Fusion.Children and string.sub(key, 1, 15) ~= "__fusion_event_" then
				writable[key] = value
			end
		end

		local children = properties[Fusion.Children]
		if children ~= nil then
			for _, child in children do
				(child :: Instance).Parent = object
			end
		end

		for key, callback in properties do
			if string.sub(key, 1, 15) == "__fusion_event_" then
				local eventName = string.sub(key, 16)
				local signal = writable[eventName] :: any
				signal:Connect(callback)
			end
		end

		return object
	end
end

return Fusion
