-- wrk only prints p50/p75/p90/p99 normally; retain required percentiles explicitly.
done = function(summary, latency, requests)
    io.write(string.format('H2O_PHP_METRICS {"requests":%d,"duration_us":%d,"rps":%.4f,"p50_ms":%.6f,"p95_ms":%.6f,"p99_ms":%.6f,"errors_connect":%d,"errors_read":%d,"errors_write":%d,"errors_status":%d,"errors_timeout":%d}\n',
        summary.requests, summary.duration, summary.requests / (summary.duration / 1000000),
        latency:percentile(50) / 1000, latency:percentile(95) / 1000, latency:percentile(99) / 1000,
        summary.errors.connect, summary.errors.read, summary.errors.write, summary.errors.status, summary.errors.timeout))
end
