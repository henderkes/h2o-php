<?php
$cache = function_exists('opcache_get_status') ? opcache_get_status(false) : false;
$normalize = static function ($value) use (&$normalize) {
    if (is_float($value) && !is_finite($value)) {
        return is_nan($value) ? 'NaN' : ($value > 0 ? 'Infinity' : '-Infinity');
    }
    if (is_array($value)) {
        foreach ($value as $key => $item) { $value[$key] = $normalize($item); }
    }
    return $value;
};
header('Content-Type: application/json');
echo json_encode($normalize([
    'php' => PHP_VERSION, 'zts' => PHP_ZTS, 'sapi' => PHP_SAPI,
    'pid' => getmypid(), 'output_buffering' => ini_get('output_buffering'),
    'opcache_enable' => ini_get('opcache.enable'), 'opcache_enable_cli' => ini_get('opcache.enable_cli'),
    'opcache_jit' => ini_get('opcache.jit'), 'opcache_jit_buffer_size' => ini_get('opcache.jit_buffer_size'),
    'opcache_enabled' => $cache['opcache_enabled'] ?? false,
    'opcache_memory' => $cache['memory_usage'] ?? null,
    'opcache_statistics' => $cache['opcache_statistics'] ?? null,
]), JSON_THROW_ON_ERROR);
