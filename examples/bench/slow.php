<?php
header('Content-Type: application/octet-stream');
while (ob_get_level() > 0) { ob_end_flush(); }
$chunk = str_repeat('r', 16384);
for ($i = 0; $i < 1024; ++$i) { echo $chunk; flush(); }
