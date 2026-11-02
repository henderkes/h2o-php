<?php
header('Content-Type: application/octet-stream');
header('X-Accel-Buffering: no');
while (ob_get_level() > 0) { ob_end_flush(); }
$chunk = str_repeat('s', 16384);
for ($i = 0; $i < 128; ++$i) { echo $chunk; flush(); }
