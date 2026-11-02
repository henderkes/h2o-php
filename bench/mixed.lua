local paths = {'/static.txt', '/minimal.php', '/worker', '/json.php'}
local index = 0
request = function()
    index = index % #paths + 1
    return wrk.format(nil, paths[index])
end
dofile(os.getenv('BENCH_LATENCY_LUA') or 'bench/latency.lua')
