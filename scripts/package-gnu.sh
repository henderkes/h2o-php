#!/usr/bin/env bash
# Build/stage an auditable GNU artifact; ELF inspection determines staticness.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 - "$project_root" "$@" <<'PY'
import argparse,hashlib,json,os,pathlib,shutil,subprocess,tarfile,time
root=pathlib.Path(__import__('sys').argv[1]);p=argparse.ArgumentParser()
p.add_argument('--work',type=pathlib.Path,default=root/'build/distribution')
p.add_argument('--stage',type=pathlib.Path,help='Stage into a fresh directory without replacing any existing entry; failed validation may leave an incomplete new stage')
p.add_argument('--cache-prefix',type=pathlib.Path,default=pathlib.Path('/home/m/static-php-cli/buildroot'))
p.add_argument('--dependency-source',type=pathlib.Path,default=pathlib.Path('/home/m/static-php-cli/source'))
p.add_argument('--jobs',type=int,default=4);p.add_argument('--build',action='store_true')
p.add_argument('--skip-php-build',action='store_true');p.add_argument('--manifest-only',action='store_true')
p.add_argument('--fully-static',action='store_true',help='Require no PT_INTERP or DT_NEEDED; requires matching static libc/toolchain')
p.add_argument('--libc-prefix',type=pathlib.Path,help='Optional extracted matching GNU libc-static prefix')
p.add_argument('--functional-evidence',type=pathlib.Path,help='Finite functional result JSON bound to this exact artifact SHA256')
a=p.parse_args(__import__('sys').argv[2:]);work=a.work.resolve();cache=a.cache_prefix.resolve();php=work/'php-static-gnu';build=work/('h2o-gnu-static' if a.fully_static else 'h2o-gnu');stage=a.stage if a.stage else work/('gnu-static-artifact' if a.fully_static else 'gnu-artifact')
if not stage.is_absolute():stage=pathlib.Path.cwd()/stage
if not 1<=a.jobs<=32:p.error('--jobs must be 1..32')
# Check the requested entry itself, including dangling final symlinks. Plan-only
# invocations do not materialize a stage and may still describe an existing one.
if (a.build or a.manifest_only) and os.path.lexists(stage):raise SystemExit('Refusing existing stage; choose a new --stage directory: '+str(stage))
def run(argv,**kw):return subprocess.run([str(x) for x in argv],check=True,**kw)
def output(argv):return subprocess.check_output([str(x) for x in argv],text=True).strip()
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
work.mkdir(parents=True,exist_ok=True)
env=os.environ.copy();env.update(PKG_CONFIG_LIBDIR=f'{php}/prefix/lib/pkgconfig:{cache}/lib/pkgconfig',LC_ALL='C',TZ='UTC')
args=['cmake','-S',root,'-B',build,f'-DCMAKE_TOOLCHAIN_FILE={root}/cmake/static-gnu.cmake',f'-DH2O_PHP_STATIC_DEPS={cache}',f'-DH2O_PHP_STATIC_PHP={php}/prefix','-DCMAKE_BUILD_TYPE=Release','-DH2O_PHP_PREFER_EMBEDDED_CA=ON',f'-DPKG_CONFIG_EXECUTABLE={php}/pkg-config-static']
if a.fully_static:
 stdcpp=pathlib.Path(output(['g++','-print-file-name=libstdc++.a'])).resolve()
 if not stdcpp.is_file():raise SystemExit('Matching static libstdc++.a is required for CMake compiler validation')
 flags=f'-static -fuse-ld=lld -Wl,--gc-sections -L{php}/prefix/lib -L{cache}/lib -L{stdcpp.parent}'
 if a.libc_prefix:
  libc=a.libc_prefix.resolve()
  # RPM linker scripts contain installation-absolute paths; relocate only the workspace copy.
  math=libc/'usr/lib64/libm.a'
  if math.exists() and math.read_bytes().startswith(b'/* GNU ld script'):
   original=math.with_suffix('.a.original')
   if not original.exists():shutil.copy2(math,original)
   math.write_text(original.read_text().replace('/usr/lib64/',str(libc/'usr/lib64')+'/'))
  flags+=f' -L{libc}/usr/lib64 -L{libc}/lib64'
 args += ['-DCMAKE_EXE_LINKER_FLAGS='+flags]
if not a.manifest_only:
 if not a.build:
  print('No compilation executed. Add --build to build the pinned PHP archive and GNU H2O artifact.')
  print('CMake argv: '+json.dumps([str(x) for x in args]));raise SystemExit(0)
 if not a.skip_php_build:
  php_args=[root/'scripts/package-php-static.sh','--work',php,'--cache-prefix',cache,'--jobs',a.jobs,'--build']
  if php.exists():php_args+=['--resume']
  run(php_args)
 if not (php/'result.json').is_file():raise SystemExit('Missing successful static PHP build/probe result: '+str(php/'result.json'))
 with (work/(build.name+'-configure.log')).open('a') as log:run(args,env=env,stdout=log,stderr=subprocess.STDOUT)
 with (work/(build.name+'-build.log')).open('a') as log:run(['cmake','--build',build,'--target','h2o','-j'+str(a.jobs)],env=env,stdout=log,stderr=subprocess.STDOUT)
binary=build/'h2o-php'
if not binary.is_file():raise SystemExit('Artifact missing: '+str(binary))
result=json.loads((php/'result.json').read_text())
# Exclusive creation is authoritative if an entry appeared after the early
# guard. Validation failures may leave this new stage as incomplete evidence;
# publishing the package is not claimed to be atomic.
stage.mkdir(mode=0o700,parents=True,exist_ok=False);shutil.copy2(binary,stage/'h2o-php')
elf=output(['readelf','-W','-l',binary]);dynamic=run(['readelf','-W','-d',binary],capture_output=True,text=True).stdout
needed=[]
for line in dynamic.splitlines():
 if '(NEEDED)' in line:needed.append(line.split('[',1)[1].split(']',1)[0])
interp=next((line.strip() for line in elf.splitlines() if 'Requesting program interpreter' in line),None)
fully_static=not needed and interp is None
(stage/'readelf-program-headers.txt').write_text(elf+'\n');(stage/'readelf-dynamic.txt').write_text(dynamic)
if a.fully_static and not fully_static:raise SystemExit('Static profile still requires runtime loader/libraries: '+json.dumps({'interpreter':interp,'needed':needed}))
version=output([binary,'--version']);(stage/'version.txt').write_text(version+'\n')
cli=output([binary,'php-cli','-n','-r',"echo json_encode(['version'=>PHP_VERSION,'zts'=>PHP_ZTS,'pcntl'=>extension_loaded('pcntl'),'parallel'=>extension_loaded('parallel'),'result'=>parallel\\run(static fn()=>42)->value()]);"])
probe=json.loads(cli)
if probe!={'version':'8.6.0RC4','zts':1,'pcntl':True,'parallel':True,'result':42}:raise SystemExit('Unexpected final executable CLI result: '+cli)
(stage/'cli-probe.json').write_text(json.dumps(probe,indent=2)+'\n')
licenses=stage/'licenses';licenses.mkdir(exist_ok=True);notice_records=[]
def notices(label,source):
 if not source.exists():return
 candidates=[source] if source.is_file() else [x for x in source.rglob('*') if x.is_file() and x.name.upper().startswith(('LICENSE','LICENCE','COPYING','NOTICE','COPYRIGHT')) and not any(y in x.parts for y in ('.git','.libs','tests'))]
 for src in candidates:
  rel=src.name if source.is_file() else str(src.relative_to(source));dest=licenses/label/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(src,dest)
  notice_records.append({'component':label,'path':str(dest.relative_to(stage)),'sha256':sha(dest)})
notices('h2o',root/'upstream/h2o');notices('php',root/'upstream/php-src-8.6');notices('frankenphp',root/'src/php/LICENSE.frankenphp');notices('parallel',root/'upstream/parallel');notices('embedded-ca',root/'assets')
for name in ['openssl','zlib','libxml2','libiconv','libsodium','brotli','zstd']:notices(name,a.dependency_source/name)
notices('sqlite',php/'sqlite-source')
if a.fully_static:
 notices('glibc',pathlib.Path('/usr/share/licenses/glibc'))
 notices('gcc-runtime',pathlib.Path('/usr/share/licenses/gcc'))
(stage/'licenses.json').write_text(json.dumps(notice_records,indent=2)+'\n')
source_files=[]
for folder in ['src','include','cmake','scripts','patches','examples','assets','resources']:
 source_files.extend(x for x in (root/folder).rglob('*') if x.is_file())
source_files += [root/'CMakeLists.txt',root/'CMakePresets.json']
identity={str(x.relative_to(root)):sha(x) for x in sorted(source_files) if x.is_file()}
archives=[cache/'lib'/('lib'+name+'.a') for name in ['ssl','crypto','z','xml2','iconv','sodium','brotlidec','brotlienc','brotlicommon','zstd']]
archives += [php/'prefix/lib/libphp.a',php/'prefix/lib/libsqlite3.a']
if a.fully_static and a.libc_prefix:archives += sorted(a.libc_prefix.resolve().rglob('*.a'))
archive_hashes={str(x):sha(x) for x in archives if x.is_file()}
evidence=None
if a.functional_evidence:
 evidence=json.loads(a.functional_evidence.read_text())
 if evidence.get('artifact_sha256')!=sha(binary):raise SystemExit('Functional evidence refers to a different executable; rerun the finite smoke check')
 shutil.copy2(a.functional_evidence,stage/'functional-evidence.json')
manifest={'artifact':{'file':'h2o-php','sha256':sha(binary),'size':binary.stat().st_size,'fully_static_elf':fully_static,'interpreter':interp,'needed':needed},'php':result,'h2o_revision':output(['git','-C',root/'upstream/h2o','rev-parse','HEAD']),'project_head':output(['git','-C',root,'rev-parse','HEAD']),'project_dirty':bool(output(['git','-C',root,'status','--porcelain'])),'workspace_file_sha256_at_packaging':identity,'compiled_version_report':version,'cmake_argv':[str(x) for x in args],'compiler':output(['clang','--version']).splitlines()[0],'cached_dependency_cpu_profile':'GNU x86-64-v3 LLVM21.1 ThinLTO; this profile is not a generic x86-64 or musl build','functional_cli_probe':probe,'embedded_ca_preferred':True,'benchmark_performed':False,'functional_evidence':'functional-evidence.json' if evidence else None,'license_files':'licenses.json','static_archive_sha256':archive_hashes,'php_module_inventory':output([binary,'php-cli','-n','-m']),'glibc_static_input':json.loads((a.libc_prefix/'input.json').read_text()) if a.fully_static and a.libc_prefix else None,'deployment_notes':['Application code, YAML, certificates and writable application data remain external.','GNU distribution prefers pinned embedded CA data; explicit trust overrides retain precedence.','GNU libc compatibility and native extension fault/process isolation constraints still apply.']}
(stage/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
shutil.copy2(root/'scripts/package-php-static.sh',stage/'package-php-static.sh');shutil.copy2(root/'scripts/package-gnu.sh',stage/'package-gnu.sh');shutil.copy2(root/'scripts/package-glibc-static.sh',stage/'package-glibc-static.sh')
print(json.dumps({'artifact':str(stage/'h2o-php'),'fully_static_elf':fully_static,'needed':needed,'manifest':str(stage/'manifest.json')},indent=2))
PY
