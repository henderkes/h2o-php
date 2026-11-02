<?php
$path = parse_url($_SERVER['REQUEST_URI'], PHP_URL_PATH);
if ($path === '/info') {
    header('Link: </style.css>; rel=preload; as=style');
    headers_send(103);
}
if ($path === '/stream') {
    header('Content-Type: text/plain');
    for ($i=0; $i<4; ++$i) { echo "chunk$i\n"; if (ob_get_level()) ob_flush(); flush(); usleep(100000); }
    return;
}
if ($path === '/finish') { echo "finished\n"; frankenphp_finish_request(); usleep(400000); file_put_contents('/tmp/hp-smoke-finish', 'done'); return; }
if ($path === '/upload') { header('Content-Type: application/json'); echo json_encode(['post'=>$_POST,'files'=>array_map(fn($f)=>['name'=>$f['name'],'size'=>$f['size'],'error'=>$f['error'],'sha256'=>hash_file('sha256',$f['tmp_name'])],$_FILES)]); return; }
if ($path === '/body') { $body = file_get_contents('php://input'); header('Content-Type: application/json'); echo json_encode(['length'=>strlen($body),'sha256'=>hash('sha256',$body),'post'=>$_POST]); return; }
header('Content-Type: application/json');
header('Set-Cookie: first=one', false); header('Set-Cookie: second=two', false);
echo json_encode(['sapi'=>PHP_SAPI,'extension'=>extension_loaded('frankenphp'),'uri'=>$_SERVER['REQUEST_URI'],'query'=>$_GET,'root'=>$_SERVER['DOCUMENT_ROOT'],'script'=>$_SERVER['SCRIPT_FILENAME'],'script_name'=>$_SERVER['SCRIPT_NAME'],'path_info'=>$_SERVER['PATH_INFO']??'', 'headers'=>getallheaders()]);
