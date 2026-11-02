from pathlib import Path
import hashlib,json,socket,subprocess,time
root=Path('/home/m/h2o-php');work=Path('/tmp/hp-queue-deadline')
(work/'loop.php').write_text("<?php while(frankenphp_handle_request(function(){while(true){}})) {}")
config=(work/'h2o.yml').read_text().replace('18130','18131').replace('shutdown-timeout: 1s','shutdown-timeout: 200ms').replace('worker.php','loop.php')
(work/'shutdown.yml').write_text(config);log=open(work/'shutdown-server.log','w');binary=root/'build/release/h2o-php'
p=subprocess.Popen([str(binary),'-c',str(work/'shutdown.yml')],stdout=log,stderr=log);client=None
try:
    for _ in range(100):
        try:
            client=socket.create_connection(('127.0.0.1',18131),timeout=.1);break
        except OSError:
            if p.poll() is not None:raise RuntimeError('startup failed')
            time.sleep(.01)
    client.sendall(b'GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n');time.sleep(.05)
    began=time.monotonic();p.terminate();p.wait(timeout=3)
    result={'requests':1,'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'shutdown_budget_ms':200,'shutdown_elapsed_ms':round((time.monotonic()-began)*1000,2),'exit':p.returncode}
finally:
    if client:client.close()
    if p.poll() is None:p.kill();p.wait()
    log.close()
result['all_loops_drained']='2 network loops drained, 0 pending completions' in (work/'shutdown-server.log').read_text()
(work/'shutdown.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result))
assert result['exit']==0 and result['all_loops_drained'] and result['shutdown_elapsed_ms']<500
