import os,subprocess,time,socket,tempfile
from pathlib import Path
# Run from the repository root. This is a profiling driver, not a test suite.
mode=os.environ.get('HP_PROFILE','callgrind');port=18110 if mode=='callgrind' else 18109
if mode not in ('callgrind', 'memcheck'):
 raise SystemExit('HP_PROFILE must be callgrind or memcheck')
Path('bench/results/runtime').mkdir(parents=True, exist_ok=True)
temporary=tempfile.TemporaryDirectory(prefix='h2o-php-profile-')
certificate=Path(temporary.name)/'certificate.pem';key=Path(temporary.name)/'key.pem'
subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-keyout',str(key),'-out',str(certificate),'-subj','/CN=localhost','-days','1'],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
root=Path('examples/smoke').resolve();config=Path(temporary.name)/'h2o.yml'
config.write_text(f"""listen:
  port: {port}
  ssl:
    certificate-file: {certificate}
    key-file: {key}
num-threads: 2
php.runtime:
  threads: 2
  max-threads: 8
  request-timeout: 5s
php.root: {root}
php.ini:
  output_buffering: 0
  opcache.jit: 0
  opcache.jit_buffer_size: 0
hosts:
  \"localhost:{port}\":
    paths:
      \"/worker\":
        php.worker:
          file: {root}/worker.php
          num: 1
      \"/\":
        file.dir: {root}
        file.custom-handler:
          extension: .php
          php.classic: ON
""")
opts=['--tool=callgrind','--instr-atstart=no','--callgrind-out-file=bench/results/runtime/callgrind.out'] if mode=='callgrind' else ['--tool=memcheck','--leak-check=no','--undef-value-errors=no','--num-callers=20']
args=['valgrind',*opts,f'--log-file=bench/results/runtime/{mode}-tls.txt','build/release/h2o-php','-c',str(config)]
print('command',' '.join(args),flush=True)
log=open(f'bench/results/runtime/{mode}-tls-server.log','w')
env=os.environ.copy();env['USE_ZEND_ALLOC']='0'
p=subprocess.Popen(args,stdout=log,stderr=log,env=env)
try:
 for i in range(300):
  try:s=socket.create_connection(('localhost',port),timeout=.1);s.close();break
  except OSError:
   if p.poll() is not None:raise RuntimeError('profile process exited')
   time.sleep(.05)
 urls=[f'https://localhost:{port}/index.php',f'https://localhost:{port}/worker']
 warmup=subprocess.run(['curl','-ksS','--http2','--max-time','20',*urls],capture_output=True,text=True)
 print('warmup',warmup.returncode,warmup.stdout[:500],warmup.stderr,flush=True)
 if mode=='callgrind':print(subprocess.run(['callgrind_control','-i','on',str(p.pid)],capture_output=True,text=True).stdout,flush=True)
 measured=subprocess.run(['curl','-ksS','--http2','--max-time','20',*(urls*25)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE,text=True)
 print('measured50 requests exit',measured.returncode,measured.stderr,flush=True)
 if mode=='callgrind':print(subprocess.run(['callgrind_control','-d',str(p.pid)],capture_output=True,text=True).stdout,flush=True)
finally:
 p.terminate()
 try:p.wait(timeout=10)
 except subprocess.TimeoutExpired:p.kill();p.wait();print('shutdown killed',flush=True)
 print('server exit',p.returncode,flush=True);log.close();temporary.cleanup()
