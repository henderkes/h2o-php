<?php
$items = [];
for ($i = 0; $i < 100; ++$i) {
    $items[] = ['id' => $i, 'hash' => hash('sha256', (string) $i), 'value' => $i * $i];
}
header('Content-Type: application/json');
echo json_encode(['items' => $items], JSON_THROW_ON_ERROR);
