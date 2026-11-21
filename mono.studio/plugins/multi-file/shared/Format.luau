--!strict
local Format = {}

function Format.Count(count: number): string
	return "pressed " .. tostring(count) .. " time(s)"
end

return Format
