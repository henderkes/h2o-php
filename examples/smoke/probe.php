<?php
$action = $_GET['action'] ?? 'identity';
switch ($action) {
case 'identity':
    header('Content-Type: application/json');
    $functions = ['frankenphp_handle_request','frankenphp_finish_request','fastcgi_finish_request','frankenphp_log','frankenphp_request_headers','frankenphp_response_headers','apache_request_headers','apache_response_headers','getallheaders','headers_send'];
    $metadata = [];
    foreach ($functions as $name) {
        $r = new ReflectionFunction($name);
        $metadata[$name] = ['return'=>(string)$r->getReturnType(),'required'=>$r->getNumberOfRequiredParameters(),'parameters'=>array_map(fn($p)=>[$p->getName(),(string)$p->getType(),$p->isDefaultValueAvailable()?$p->getDefaultValue():null],$r->getParameters())];
    }
    echo json_encode(['sapi'=>php_sapi_name(),'constant'=>PHP_SAPI,'extension'=>extension_loaded('frankenphp'),'metadata'=>$metadata,'mercure'=>function_exists('mercure_publish'),'opcache'=>function_exists('opcache_get_status')?opcache_get_status(false):false,'env'=>getenv('HP_PROBE_PRIVATE')], JSON_THROW_ON_ERROR);
    break;
case 'fatal': undefined_probe_function(); break;
case 'exit': echo 'before exit'; exit(7);
case 'worker-outside':
    try { frankenphp_handle_request(fn()=>null); }
    catch (Throwable $e) { echo get_class($e).':'.$e->getMessage(); }
    break;
case 'env':
    header('Content-Type: application/json');
    $before=getenv('HP_PROBE_PRIVATE');
    if (isset($_GET['value'])) putenv('HP_PROBE_PRIVATE='.$_GET['value']);
    usleep((int)($_GET['sleep']??0));
    echo json_encode(['before'=>$before,'after'=>getenv('HP_PROBE_PRIVATE'),'env'=>$_ENV['HP_PROBE_PRIVATE']??null]);
    break;
case 'headers':
    header('Set-Cookie: a=1',false); header('Set-Cookie: b=2',false);
    header('X-Probe: yes');
    echo json_encode([getallheaders(),apache_request_headers(),frankenphp_request_headers(),apache_response_headers(),frankenphp_response_headers()]);
    break;
case 'finish':
    header('X-Finished: yes'); echo 'early';
    $a=fastcgi_finish_request();$b=frankenphp_finish_request();
    usleep(300000);
    file_put_contents('/tmp/hp-probe-finish',json_encode([$a,$b]));
    echo 'must not be sent';
    break;
case 'log-object':
    try { frankenphp_log('invalid context',0,['object'=>new stdClass]); }
    catch(Throwable $e){echo get_class($e).':'.$e->getMessage();} break;
case 'log-recursive':
    $a=[];$a['recursive']=&$a;
    try { frankenphp_log('invalid recursion',0,$a); }
    catch(Throwable $e){echo get_class($e).':'.$e->getMessage();} break;
case 'invalid-status': headers_send(-5); echo 'clamped';break;
case 'log':
    frankenphp_log('structured smoke',FRANKENPHP_LOG_LEVEL_WARN,['string'=>'test','bool'=>true,'number'=>42,'nested'=>['x'=>[1,2]]]);echo 'logged';break;
case 'loop': while(true) {};
case 'session':
    session_start(); echo json_encode(['id'=>session_id(),'old'=>$_SESSION['value']??null]); $_SESSION['value']='stored'; break;
case 'upload':
    header('Content-Type: application/json');echo json_encode(['post'=>$_POST,'files'=>$_FILES,'input_len'=>strlen(file_get_contents('php://input'))]);break;
case 'opcache-reset': var_export(opcache_reset()); break;
case 'shutdown':register_shutdown_function(fn()=>print('shutdown'));echo 'body-';break;
}
