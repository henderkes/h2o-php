<?php
header('Content-Type: text/plain');
$input = fopen('php://input', 'r');
$first = fread($input, 8192);
echo 'first='.strlen($first)."\n";
flush();
$rest = stream_get_contents($input);
echo 'total='.(strlen($first)+strlen($rest))."\n";
