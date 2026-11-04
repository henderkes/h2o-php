#!/usr/bin/env bash
# Extract, never install, the exact GNU libc archive used by this local profile.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
exec python3 - "$project_root" "$@" <<'PY'
import argparse,hashlib,json,pathlib,subprocess
root=pathlib.Path(__import__('sys').argv[1]);p=argparse.ArgumentParser()
p.add_argument('--work',type=pathlib.Path,default=root/'build/distribution');p.add_argument('--fetch',action='store_true')
p.add_argument('--fetch-source',action='store_true',help='Also fetch/verify matching signed Alma source RPM for the local relink bundle')
a=p.parse_args(__import__('sys').argv[2:]);work=a.work.resolve();work.mkdir(parents=True,exist_ok=True)
name='glibc-static-2.39-145.el10_2.alma.1.x86_64.rpm';checksum='d679351abb7d45d9436223f4a71b47d1cac334cbe57999f272540fd059f267c2'
url='https://repo.almalinux.org/almalinux/10/CRB/x86_64/os/Packages/'+name;rpm=work/name
if not rpm.exists():
 if not a.fetch:raise SystemExit('Missing '+str(rpm)+'; --fetch downloads the pinned official package without installing it.')
 subprocess.run(['curl','--fail','--location','--retry','2','--output',str(rpm),url],check=True)
if hashlib.sha256(rpm.read_bytes()).hexdigest()!=checksum:raise SystemExit('Checksum mismatch for '+str(rpm))
sig=subprocess.check_output(['rpm','-K',str(rpm)],text=True).strip()
out=work/'glibc-static';out.mkdir(exist_ok=True)
proc=subprocess.Popen(['rpm2cpio',str(rpm)],stdout=subprocess.PIPE)
subprocess.run(['cpio','-idmu','--quiet'],stdin=proc.stdout,cwd=out,check=True)
if proc.wait():raise SystemExit('rpm2cpio failed')
(out/'input.json').write_text(json.dumps({'package':name,'sha256':checksum,'url':url,'checksum_source':'https://errata.almalinux.org/10/ALSA-2026-84753.html','signature':sig},indent=2)+'\n')
if a.fetch_source:
 source_name='glibc-2.39-145.el10_2.alma.1.src.rpm';source=work/source_name
 source_sha='7760580fd0ae738cc7d2415a00f6e740c3d6ec1af164a9f4d35ae5ed645fbf42'
 source_url='https://linuxsoft.cern.ch/cern/alma/10-testing/BaseOS/source/Packages/'+source_name
 if not source.exists():subprocess.run(['curl','--fail','--location','--retry','2','--output',str(source),source_url],check=True)
 if hashlib.sha256(source.read_bytes()).hexdigest()!=source_sha:raise SystemExit('Source RPM checksum mismatch')
 source_sig=subprocess.check_output(['rpm','-K',str(source)],text=True).strip()
 (work/'glibc-source-input.json').write_text(json.dumps({'package':source_name,'sha256':source_sha,'url':source_url,'signature':source_sig},indent=2)+'\n')
print(out)
PY
