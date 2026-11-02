local size = tonumber(os.getenv('BENCH_BODY_SIZE') or '1024')
wrk.method = 'POST'
wrk.headers['Content-Type'] = 'application/octet-stream'
wrk.body = string.rep('p', size)
dofile(os.getenv('BENCH_LATENCY_LUA') or 'bench/latency.lua')
