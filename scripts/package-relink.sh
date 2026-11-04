#!/usr/bin/env bash
# Preserve local static-link objects, archives and optional source snapshots.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 - "$project_root" "$@" <<'PY'
import argparse,hashlib,json,os,pathlib,shlex,shutil,subprocess,tarfile
root=pathlib.Path(__import__('sys').argv[1]);p=argparse.ArgumentParser()
p.add_argument('--work',type=pathlib.Path,default=root/'build/distribution')
p.add_argument('--stage',type=pathlib.Path,help='Use a separately staged artifact and preserve earlier relink kits')
p.add_argument('--cache-prefix',type=pathlib.Path,default=pathlib.Path('/home/m/static-php-cli/buildroot'))
p.add_argument('--dependency-source',type=pathlib.Path,default=pathlib.Path('/home/m/static-php-cli/source'))
p.add_argument('--sources',action='store_true');p.add_argument('--verify',action='store_true')
a=p.parse_args(__import__('sys').argv[2:]);work=a.work.resolve();build=work/'h2o-gnu-static';stage=a.stage.resolve() if a.stage else work/'gnu-static-artifact';kit=stage/'relink-kit';kit.mkdir(parents=True,exist_ok=True)
for name in ['objects','archives','compiler-support','sources','inputs']:(kit/name).mkdir(exist_ok=True)
def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def out(argv):return subprocess.check_output([str(x) for x in argv],text=True).strip()
link=build/'h2o/CMakeFiles/h2o.dir/link.txt';original=shlex.split(link.read_text());search=[pathlib.Path(x[2:]) for x in original if x.startswith('-L')]
search += [pathlib.Path(x) for x in out(['clang','--print-search-dirs']).split('libraries: =',1)[-1].split(':')]
search += [pathlib.Path('/usr/lib64'),pathlib.Path('/lib64')]
records=[];arguments=[];object_index=0

def save(src,category,name=None):
 src=src.resolve();dest=kit/category/(name or src.name)
 if dest.exists() and sha(dest)!=sha(src):
  # GNU libm's copied linker script is intentionally relocated below.
  if dest.name!='libm.a':dest.unlink()
 shutil.copy2(src,dest)
 records.append({'source':str(src),'path':str(dest.relative_to(kit)),'sha256':sha(src)})
 return str(dest.relative_to(kit))

def library(name):
 for directory in search:
  path=directory/('lib'+name+'.a')
  if path.is_file():return path
 raise SystemExit('Missing static linker input -l'+name)

i=1
while i<len(original):
 arg=original[i]
 if arg.startswith('-L'):i+=1;continue
 if arg=='-Xlinker' and i+1<len(original) and original[i+1].startswith('--dependency-file='):i+=2;continue
 if arg=='-o':arguments+=['-o','h2o-php.relinked'];i+=2;continue
 if arg.endswith('.o'):
  path=pathlib.Path(arg);path=path if path.is_absolute() else build/'h2o'/path
  arguments.append(save(path,'objects',f'{object_index:04d}-'+path.name));object_index+=1
 elif arg.endswith('.a'):arguments.append(save(pathlib.Path(arg),'archives'))
 elif arg.startswith('-l'):arguments.append(save(library(arg[2:]),'archives'))
 else:arguments.append(arg)
 i+=1
# The C driver appends these itself. Preserve their actual matching originals.
for name in ['crt1.o','crti.o','crtbeginT.o','crtend.o','crtn.o','libgcc.a','libgcc_eh.a']:
 path=pathlib.Path(out(['clang','-print-file-name='+name]))
 if not path.is_file():raise SystemExit('Missing compiler support '+name)
 save(path,'compiler-support')
for name in ['libc.a','libm.a','libm-2.39.a','libmvec.a','libresolv.a']:
 path=work/'glibc-static/usr/lib64'/name
 if path.is_file():save(path,'archives')
math=kit/'archives/libm.a'
if math.exists() and math.read_bytes().startswith(b'/* GNU ld script'):
 math.write_text('/* Workspace relocation of the original GNU libm linker script. */\nOUTPUT_FORMAT(elf64-x86-64)\nGROUP ( -l:libm-2.39.a -l:libmvec.a )\n')
for category in ['archives','compiler-support']:
 for path in (kit/category).iterdir():
  if path.suffix=='.a' and category=='compiler-support':shutil.copy2(path,kit/'archives'/path.name)
(kit/'link-arguments.json').write_text(json.dumps(arguments,indent=2)+'\n');shutil.copy2(link,kit/'inputs/original-link.txt')
for path in [work/'php-static-gnu/inputs.json',work/'php-static-gnu/result.json',work/'glibc-static/input.json',work/'glibc-source-input.json',stage/'manifest.json']:
 if path.exists():shutil.copy2(path,kit/'inputs'/(path.parent.name+'-'+path.name))
for path in root.glob('scripts/package*.sh'):shutil.copy2(path,kit/'inputs'/path.name)
(kit/'relink.sh').write_text('''#!/usr/bin/env bash
set -euo pipefail
kit_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec python3 - "$kit_root" <<'PYLINK'
import json,os,pathlib,subprocess,hashlib
kit=pathlib.Path(__import__('sys').argv[1]);args=json.loads((kit/'link-arguments.json').read_text())
cmd=[os.environ.get('CC','clang'),'-B'+str(kit/'compiler-support'),'-L'+str(kit/'archives'),*args]
subprocess.run(cmd,cwd=kit,check=True)
binary=kit/'h2o-php.relinked';print('Relinked SHA256:',hashlib.sha256(binary.read_bytes()).hexdigest())
subprocess.run([str(binary),'php-cli','-n','-r',"echo json_encode(['zts'=>PHP_ZTS,'parallel'=>parallel\\\\run(static fn()=>42)->value()]),PHP_EOL;"],check=True)
PYLINK
''');(kit/'relink.sh').chmod(0o755)
source_records=json.loads((kit/'manifest.json').read_text()).get('source_materials',[]) if (kit/'manifest.json').exists() and not a.sources else []
if a.sources:
 def snapshot(name,path):
  destination=kit/'sources'/(name+'.tar.gz')
  def select(info):
   parts=pathlib.PurePosixPath(info.name).parts
   if any(x in parts for x in ('.git','.libs','CMakeFiles','autom4te.cache','__pycache__')):return None
   if info.isfile() and info.name.endswith(('.o','.lo','.a','.so','.la','.d','.dep')):return None
   return info
  if not destination.exists() or name=='h2o':
   with tarfile.open(destination,'w:gz',compresslevel=2) as archive:archive.add(path,arcname=name,filter=select)
  source_records.append({'name':name,'path':str(destination.relative_to(kit)),'sha256':sha(destination),'kind':'source-tree snapshot, generated object files excluded'})
 snapshot('h2o',root/'upstream/h2o')
 for name in ['openssl','zlib','libxml2','libiconv','libsodium','brotli','zstd']:
  path=a.dependency_source/name
  if path.exists():snapshot(name,path)
 for name,repo,revision in [('php',root/'upstream/php-src-8.6','f23b4c222cebe33be93ee8ecaaa14390c5507a08'),('parallel-base',root/'upstream/parallel','4edfdc2e40070b1d3ea41a92541e9fadf29725c2')]:
  destination=kit/'sources'/(name+'.tar.gz');subprocess.run(['git','-C',str(repo),'archive','--format=tar.gz','--prefix='+name+'/',revision,'-o',str(destination)],check=True)
  source_records.append({'name':name,'path':str(destination.relative_to(kit)),'sha256':sha(destination),'revision':revision})
 destination=kit/'sources/native-integration.tar.gz'
 with tarfile.open(destination,'w:gz',compresslevel=2) as archive:
  for name in ['src','include','cmake','scripts','patches','examples','docs','assets','resources','CMakeLists.txt','CMakePresets.json']:
   path=root/name
   if path.exists():archive.add(path,arcname=name,filter=(lambda info: None if info.isfile() and not info.name.endswith('.md') else info) if name=='docs' else None)
 source_records.append({'name':'native-integration','path':str(destination.relative_to(kit)),'sha256':sha(destination),'kind':'workspace snapshot, including current patches/host ABI'})
 for path in [pathlib.Path('/home/m/static-php-cli/downloads/sqlite-autoconf-3450200.tar.gz'),work/'glibc-2.39-145.el10_2.alma.1.src.rpm']:
  if path.is_file():
   shutil.copy2(path,kit/'sources'/path.name);source_records.append({'name':path.name,'path':'sources/'+path.name,'sha256':sha(path)})
(kit/'README.txt').write_text('''Local GNU static relink materials, not a public-release certification.
Use a matching LLVM21.1 clang+LLD toolchain and x86-64-v3 CPU. Run ./relink.sh.
Replace an archive with a compatible rebuilt version to relink modified LGPL code.
The kit preserves final H2O objects, PHP/dependency archives, matching GNU startup
objects and compiler support, original/linkable arguments, manifests and recipes.
Optional sources/ contains matching pinned PHP/Parallel bases plus the native
patch/source snapshot and available cached dependency source trees. The cached
archives were supplied inputs; rebuilding every dependency from these snapshots
has not been qualified. GNU libc source RPM, when available, contains its precise
Alma package spec and patches. Keep all distribution notices with the executable.
Application assets and private runtime configuration are not collected here.
''')
for record in records:record['staged_sha256']=sha(kit/record['path'])
manifest={'artifact_sha256':sha(build/'h2o-php'),'inputs':records,'source_materials':source_records,'relink_script':'relink.sh','object_count':object_index,'verified':False}
(kit/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if a.verify:
 result=subprocess.run([str(kit/'relink.sh')],cwd=kit,capture_output=True,text=True);(kit/'verify.log').write_text(result.stdout+result.stderr)
 if result.returncode:raise SystemExit('Relink failed; inspect '+str(kit/'verify.log'))
 manifest['verified']=True;manifest['relinked_sha256']=sha(kit/'h2o-php.relinked');manifest['byte_identical']=manifest['relinked_sha256']==manifest['artifact_sha256']
 (kit/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'kit':str(kit),'objects':object_index,'verified':manifest['verified'],'byte_identical':manifest.get('byte_identical')},indent=2))
PY
