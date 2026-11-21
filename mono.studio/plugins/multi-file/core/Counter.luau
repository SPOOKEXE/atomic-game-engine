--!strict
local Format = require("../shared/Format")

local Counter = {}
local count = 0

function Counter.Bump(): string
	count += 1
	return Format.Count(count)
end

return Counter
