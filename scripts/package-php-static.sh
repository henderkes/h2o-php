#!/usr/bin/env bash
# Build an isolated, pinned PHP ZTS static embed archive. No system installation.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 - "$project_root" "$@" <<'PY'
import argparse,hashlib,json,os,pathlib,shutil,subprocess,tarfile,time
root=pathlib.Path(__import__('sys').argv[1]);p=argparse.ArgumentParser(description='Isolated PHP 8.6 RC4 ZTS static embed + native-host parallel; GNU dependency profile.')
p.add_argument('--work',type=pathlib.Path,default=root/'build/distribution/php-static-gnu')
p.add_argument('--cache-prefix',type=pathlib.Path,default=pathlib.Path('/home/m/static-php-cli/buildroot'))
p.add_argument('--sqlite-archive',type=pathlib.Path,default=pathlib.Path('/home/m/static-php-cli/downloads/sqlite-autoconf-3450200.tar.gz'))
p.add_argument('--jobs',type=int,default=4);p.add_argument('--build',action='store_true');p.add_argument('--resume',action='store_true')
p.add_argument('--refresh-parallel',action='store_true',help='Refresh patched parallel source and rebuild changed objects in an existing isolated build')
a=p.parse_args(__import__('sys').argv[2:]);work=a.work.resolve();prefix=work/'prefix';cache=a.cache_prefix.resolve()
if not 1<=a.jobs<=32:p.error('--jobs must be 1..32')
php_sha='f23b4c222cebe33be93ee8ecaaa14390c5507a08';parallel_sha='4edfdc2e40070b1d3ea41a92541e9fadf29725c2'
php_repo=root/'upstream/php-src-8.6';parallel_repo=root/'upstream/parallel'
def run(args,**kw):return subprocess.run([str(x) for x in args],check=True,**kw)
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def export(repo,sha,destination):
 run(['git','-C',repo,'cat-file','-e',sha+'^{commit}']);destination.mkdir(parents=True,exist_ok=True)
 proc=subprocess.Popen(['git','-C',str(repo),'archive',sha],stdout=subprocess.PIPE)
 with tarfile.open(fileobj=proc.stdout,mode='r|') as archive:archive.extractall(destination,filter='data')
 if proc.wait():raise RuntimeError('git archive failed')
if work.exists() and not a.resume:raise SystemExit('Refusing existing workdir; use --resume for this exact pinned recipe or choose another directory: '+str(work))
work.mkdir(parents=True,exist_ok=True);prefix.mkdir(exist_ok=True)
if a.refresh_parallel and not a.resume:p.error('--refresh-parallel requires --resume')
if a.refresh_parallel:
 export(parallel_repo,parallel_sha,work/'php-src/ext/parallel')
 run(['patch','-p1','--batch','--forward','-i',root/'patches/parallel-native-host.patch'],cwd=work/'php-src/ext/parallel')
 shutil.copy2(root/'src/sapi/parallel_host.h',work/'php-src/ext/parallel/php_parallel_host.h')
if not (work/'php-src').exists():
 export(php_repo,php_sha,work/'php-src');export(parallel_repo,parallel_sha,work/'php-src/ext/parallel')
 run(['patch','-p1','--batch','--forward','-i',root/'patches/parallel-native-host.patch'],cwd=work/'php-src/ext/parallel')
 shutil.copy2(root/'src/sapi/parallel_host.h',work/'php-src/ext/parallel/php_parallel_host.h')
 if not a.sqlite_archive.is_file():raise SystemExit('Missing pinned SQLite source archive: '+str(a.sqlite_archive))
 with tarfile.open(a.sqlite_archive) as archive:archive.extractall(work/'sqlite-source',filter='data')
 sqlite_dirs=list((work/'sqlite-source').iterdir());assert len(sqlite_dirs)==1
 (work/'sqlite-dir.txt').write_text(str(sqlite_dirs[0]))
cc=shutil.which('clang');cxx=shutil.which('clang++');ar=shutil.which('llvm-ar');ranlib=shutil.which('llvm-ranlib')
if not all((cc,cxx,ar,ranlib)):raise SystemExit('Matching clang/llvm-ar/llvm-ranlib are required for cached LLVM static dependencies.')
required=[cache/'lib/libssl.a',cache/'lib/libcrypto.a',cache/'lib/libz.a',cache/'lib/libxml2.a',cache/'lib/libiconv.a',cache/'lib/libsodium.a']
missing=[str(x) for x in required if not x.is_file()]
if missing:raise SystemExit('Missing static GNU dependency archives: '+', '.join(missing))
env=os.environ.copy();env.update(CC=cc,CXX=cxx,AR=ar,RANLIB=ranlib,LC_ALL='C',TZ='UTC',SOURCE_DATE_EPOCH=subprocess.check_output(['git','-C',str(php_repo),'show','-s','--format=%ct',php_sha],text=True).strip(),
 CFLAGS='-O2 -fPIC -ffunction-sections -fdata-sections -fno-omit-frame-pointer',CXXFLAGS='-O2 -fPIC -ffunction-sections -fdata-sections',
 CPPFLAGS=f'-I{prefix}/include -I{cache}/include',LDFLAGS=f'-fuse-ld=lld -Wl,--gc-sections -L{prefix}/lib -L{cache}/lib',
 PKG_CONFIG_LIBDIR=f'{prefix}/lib/pkgconfig:{cache}/lib/pkgconfig')
pkg=work/'pkg-config-static';pkg.write_text('#!/bin/sh\nexec '+shutil.which('pkg-config')+' --static "$@"\n');pkg.chmod(0o755);env['PKG_CONFIG']=str(pkg)
configure=['./configure',f'--prefix={prefix}','--disable-all','--disable-shared','--enable-static','--enable-pic','--enable-zts','--disable-zend-signals','--enable-zend-max-execution-timers',
 '--enable-cli','--disable-cgi','--disable-fpm','--disable-phpdbg','--enable-embed=static',
 '--enable-ctype','--enable-fileinfo','--enable-filter','--enable-session','--enable-tokenizer','--enable-phar','--enable-posix','--enable-pcntl','--enable-sockets','--enable-mbstring','--disable-mbregex',
 '--with-openssl','--with-sodium','--with-zlib','--with-libxml','--enable-dom','--enable-simplexml','--enable-xml','--enable-xmlreader','--enable-xmlwriter','--enable-pdo','--with-pdo-sqlite','--with-sqlite3','--enable-parallel',
 '--disable-opcache-jit',f'--with-config-file-path={prefix}/etc', '--with-config-file-scan-dir=']
record={'php_revision':php_sha,'parallel_revision':parallel_sha,'parallel_patch_sha256':digest(root/'patches/parallel-native-host.patch'),'parallel_host_header_sha256':digest(root/'src/sapi/parallel_host.h'),
 'php_version':'8.6.0RC4','php_api':20260924,'profile':'GNU static PHP embed; final ELF staticness verified separately, not inferred from .a',
 'dependencies':{str(x):digest(x) for x in required},'sqlite_archive':{'path':str(a.sqlite_archive),'sha256':digest(a.sqlite_archive)},
 'compiler':subprocess.check_output([cc,'--version'],text=True).splitlines()[0],'configure_argv':configure,'build_jobs':a.jobs,'environment':{k:env[k] for k in ['CC','CXX','AR','RANLIB','CFLAGS','CPPFLAGS','LDFLAGS','PKG_CONFIG','PKG_CONFIG_LIBDIR','SOURCE_DATE_EPOCH']}}
lock=work/'inputs.json'
if a.resume and lock.exists():
 old=json.loads(lock.read_text())
 for key in ['php_revision','parallel_revision','parallel_patch_sha256','parallel_host_header_sha256','dependencies','sqlite_archive','compiler','configure_argv']:
  if old[key]!=record[key] and not (key=='dependencies' and all(record[key].get(k)==v for k,v in old[key].items())) and not (a.refresh_parallel and key in ('parallel_patch_sha256','parallel_host_header_sha256')):raise SystemExit('Input changed for resumed build: '+key+'; use a new workdir.')
lock.write_text(json.dumps(record,indent=2)+'\n')
print('Prepared pinned PHP static recipe:',lock,flush=True)
if not a.build:print('No compilation executed. Add --build --resume to compile.');raise SystemExit(0)
log=work/'build.log'
with log.open('a') as output:
 def command(argv,cwd):
  output.write('\nCOMMAND '+json.dumps([str(x) for x in argv])+'\n');output.flush();run(argv,cwd=cwd,env=env,stdout=output,stderr=subprocess.STDOUT)
 sqlite=pathlib.Path((work/'sqlite-dir.txt').read_text())
 if not (prefix/'lib/libsqlite3.a').exists():
  command(['./configure',f'--prefix={prefix}','--disable-shared','--enable-static','--disable-readline','--disable-dynamic-extensions'],sqlite)
  command(['make','-j'+str(a.jobs)],sqlite);command(['make','install'],sqlite)
 php=work/'php-src'
 if not (php/'configure').exists():command(['./buildconf','--force'],php)
 if not (php/'Makefile').exists():command(configure,php)
 command(['make','-j'+str(a.jobs)],php)
 command(['make','install-sapi','install-cli','install-headers','install-programs','install-build'],php)
archive=prefix/'lib/libphp.a'
if not archive.is_file():raise SystemExit('Build did not install libphp.a; inspect '+str(log))
nm=subprocess.check_output(['nm','-g','--defined-only',str(archive)],text=True,stderr=subprocess.DEVNULL)
for symbol in ['do_php_cli','php_module_startup','php_tsrm_startup_ex','pcntl_module_entry','parallel_module_entry','php_parallel_set_host_hooks']:
 if not any(line.split()[-1:]==[symbol] for line in nm.splitlines()):raise SystemExit('Static archive missing '+symbol)
cli=prefix/'bin/php'
probe=run([cli,'-n','-r',"echo json_encode(['php'=>PHP_VERSION,'zts'=>PHP_ZTS,'pcntl'=>extension_loaded('pcntl'),'parallel'=>extension_loaded('parallel'),'result'=>parallel\\run(static fn()=>42)->value()]),PHP_EOL;"],capture_output=True,text=True)
(work/'cli-probe.json').write_text(probe.stdout)
value=json.loads(probe.stdout);assert value['php']=='8.6.0RC4' and value['zts'] and value['pcntl'] and value['parallel'] and value['result']==42,value
record['archive_sha256']=digest(archive);record['php_config']=str(prefix/'bin/php-config');record['cli_probe']=value
(work/'result.json').write_text(json.dumps(record,indent=2)+'\n')
print('Built and probed static PHP archive:',archive,flush=True)
PY
