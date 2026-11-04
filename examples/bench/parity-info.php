<?php
// Auxiliary baseline configuration probe; not a measured workload.
header('Content-Type: application/json');
$shared = [];
foreach (@file('/proc/self/maps', FILE_IGNORE_NEW_LINES) ?: [] as $line) {
    if (preg_match('/^([0-9a-f]+)-([0-9a-f]+)\\s+([rwxps-]{4})\\s/', $line, $matches)) {
        $bytes = hexdec($matches[2]) - hexdec($matches[1]);
        if ($matches[3][3] === 's' && $bytes === 128 * 1024 * 1024) {
            $shared[] = ['bytes' => $bytes, 'mapping' => $line];
        }
    }
}

echo json_encode([
    'php' => PHP_VERSION,
    'zts' => PHP_ZTS,
    'sapi' => PHP_SAPI,
    'pid' => getmypid(),
    'shared_128m_regions' => $shared,
    'maps_source' => '/proc/self/maps read by PHP process',
    'ini' => [
        'opcache.memory_consumption' => ini_get('opcache.memory_consumption'),
        'opcache.interned_strings_buffer' => ini_get('opcache.interned_strings_buffer'),
        'opcache.enable' => ini_get('opcache.enable'),
        'opcache.enable_cli' => ini_get('opcache.enable_cli'),
        'opcache.jit' => ini_get('opcache.jit'),
        'opcache.jit_buffer_size' => ini_get('opcache.jit_buffer_size'),
        'output_buffering' => ini_get('output_buffering'),
        'memory_limit' => ini_get('memory_limit'),
    ],
], JSON_THROW_ON_ERROR);
