<?php
$body = file_get_contents('php://input');
header('Content-Type: application/json');
echo json_encode(['length' => strlen($body), 'sha256' => hash('sha256', $body)], JSON_THROW_ON_ERROR);
