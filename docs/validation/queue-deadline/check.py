from pathlib import Path
import concurrent.futures,hashlib,http.client,json,os,socket,subprocess,sys,time
root=Path('/home/m/h2o-php');work=Path('/tmp/hp-queue-deadline')
label=sys.argv[1];mode=sys.argv[2] if len(sys.argv)>2 else 'worker'
log=open(work/(label+'-server.log'),'w');binary=Path(os.environ.get('HP_DIAGNOSTIC_BINARY',root/'build/release/h2o-php'))
p=subprocess.Popen([str(binary),'-c',str(work/('classic.yml' if mode=='classic' else 'h2o.yml'))],stdout=log,stderr=log)
def request(id):
    c=http.client.HTTPConnection('127.0.0.1',18130,timeout=3)
    started=time.monotonic()
    try:
        path='/classic.php' if mode=='classic' else '/'
        c.request('GET',path+'?id='+id,headers={'Connection':'close'});r=c.getresponse();body=r.read().decode()
        return {'id':id,'status':r.status,'elapsed_ms':round((time.monotonic()-started)*1000,2),'body':body}
    finally:c.close()
try:
    for _ in range(100):
        try:
            with socket.create_connection(('127.0.0.1',18130),timeout=.1):pass
            break
        except OSError:
            if p.poll() is not None:raise RuntimeError('startup failed')
            time.sleep(.01)
    with concurrent.futures.ThreadPoolExecutor(max_workers=1) as ex:
        busy=ex.submit(request,'busy');time.sleep(.05)
        waiting=request('waiting');active=busy.result()
    result={'label':label,'mode':mode,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'requests':2,'max_wait_ms':100,'active':active,'waiting':waiting}
finally:
    p.terminate();p.wait(timeout=4);log.close()
result['server_exit']=p.returncode
(work/(label+'.json')).write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result))
