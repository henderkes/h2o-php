<?php
// Direct smoke fixture: a real persistent session handler and request callback.
$handler = new class implements SessionHandlerInterface {
    private array $sessions = [];
    public function open(string $path,string $name):bool{return true;}
    public function close():bool{return true;}
    public function read(string $id):string|false{return $this->sessions[$id]??'';}
    public function write(string $id,string $data):bool{$this->sessions[$id]=$data;return true;}
    public function destroy(string $id):bool{unset($this->sessions[$id]);return true;}
    public function gc(int $max_lifetime):int|false{return 0;}
};
session_set_save_handler($handler,true);
$boot=bin2hex(random_bytes(6));$count=0;
$_ENV['HP_PERSISTENT_ENV']='retained';
while(frankenphp_handle_request(function()use($boot,&$count){
    $count++;
    $action=$_GET['action']??'identity';
    if($action==='throw')throw new RuntimeException('worker probe exception');
    if($action==='exit'){echo 'worker-exit';exit;}
    if($action==='finish'){
        echo 'worker-early';frankenphp_finish_request();usleep(200000);
        file_put_contents('/tmp/hp-worker-finish','continued');return;
    }
    if($action==='loop'){while(true){}}
    header('Content-Type: application/json');
    $previous=$_SERVER['HP_LEAK']??null;$_SERVER['HP_LEAK']='must reset';
    $previousRequest=$_REQUEST['leak']??null;$_REQUEST['leak']='must reset';
    if($action==='setenv')putenv('HP_WORKER_PRIVATE=script-state');
    if($action==='session'){
        session_start();$old=$_SESSION['count']??0;$_SESSION['count']=$old+1;
    }
    echo json_encode(['boot'=>$boot,'count'=>$count,'previous'=>$previous,'previous_request'=>$previousRequest,'query'=>$_GET,'post'=>$_POST,'env'=>$_ENV['HP_PERSISTENT_ENV'],'worker_env'=>getenv('HP_WORKER_PRIVATE'),'marker'=>$_SERVER['FRANKENPHP_WORKER']??null,'session_old'=>$old??null,'session_id'=>session_id(),'input'=>file_get_contents('php://input')]);
})){}
