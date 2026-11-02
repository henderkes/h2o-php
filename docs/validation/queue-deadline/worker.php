<?php
while (frankenphp_handle_request(function (): void {
    if (($_GET['id'] ?? '') === 'busy') usleep(600000);
    header('Content-Type: application/json');
    echo json_encode(['id' => $_GET['id'] ?? '']);
})) {}
