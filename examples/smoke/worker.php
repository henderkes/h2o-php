<?php
$boot = bin2hex(random_bytes(8));
$count = 0;
while (frankenphp_handle_request(function () use ($boot, &$count) {
    ++$count;
    $previous = $_SERVER['LEAK'] ?? null;
    $_SERVER['LEAK'] = 'must reset';
    $body = file_get_contents('php://input');
    header('Content-Type: application/json');
    frankenphp_log('worker request', FRANKENPHP_LOG_LEVEL_INFO, ['boot'=>$boot, 'count'=>$count]);
    echo json_encode(['boot'=>$boot,'count'=>$count,'uri'=>$_SERVER['REQUEST_URI'],'body_length'=>strlen($body),'query'=>$_GET,'previous'=>$previous]);
})) {}
