<?php
// Identical fixture for this runtime and upstream FrankenPHP.
$boot = bin2hex(random_bytes(8));
$requests = 0;
$handler = static function () use ($boot, &$requests): void {
    ++$requests;
    header('Content-Type: application/json');
    echo json_encode(['boot' => $boot, 'requests' => $requests, 'method' => $_SERVER['REQUEST_METHOD'], 'query' => $_GET], JSON_THROW_ON_ERROR);
};
while (frankenphp_handle_request($handler)) {}
