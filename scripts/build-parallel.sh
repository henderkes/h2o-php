#!/usr/bin/env bash
# Build the pinned, real parallel extension for the installed PHP ZTS ABI.
# The installed PHP and its system extension directory are never modified.
set -euo pipefail
project_root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
parallel_revision=4edfdc2e40070b1d3ea41a92541e9fadf29725c2
parallel_source="$project_root/upstream/parallel"
parallel_build="$project_root/build/extensions/parallel"
php_config=${PHPZTS_CONFIG:-php-config-zts}
phpize=${PHPIZE_ZTS:-phpize-zts}
if [[ ! -d "$parallel_source/.git" ]]; then
    git clone https://github.com/krakjoe/parallel.git "$parallel_source"
    git -C "$parallel_source" checkout "$parallel_revision"
fi
if [[ $(git -C "$parallel_source" rev-parse HEAD) != "$parallel_revision" ]]; then
    printf '%s\n' "parallel source must be pinned to $parallel_revision" >&2
    exit 1
fi
if git -C "$parallel_source" apply --reverse --check "$project_root/patches/parallel-native-host.patch" 2>/dev/null; then
    : # Already patched.
else
    git -C "$parallel_source" apply --check "$project_root/patches/parallel-native-host.patch"
    git -C "$parallel_source" apply "$project_root/patches/parallel-native-host.patch"
fi
cp "$project_root/src/sapi/parallel_host.h" "$parallel_source/php_parallel_host.h"
mkdir -p "$parallel_build"
tar -C "$parallel_source" --exclude=.git -cf - . | tar -C "$parallel_build" -xf -
cd "$parallel_build"
"$phpize"
./configure --enable-parallel --with-php-config="$(command -v "$php_config")"
make -j"${BUILD_JOBS:-4}"
python3 - "$project_root" "$php_config" "$parallel_revision" "$parallel_build/modules/parallel.so" <<'PY'
import hashlib,json,pathlib,subprocess,sys
root,config,revision,module=sys.argv[1:]
p=pathlib.Path(module)
record={'upstream_revision':revision,'php_version':subprocess.check_output([config,'--version'],text=True).strip(),
        'php_config':config,'module':str(p),'module_sha256':hashlib.sha256(p.read_bytes()).hexdigest(),
        'patch_sha256':hashlib.sha256((pathlib.Path(root)/'patches/parallel-native-host.patch').read_bytes()).hexdigest(),
        'host_abi_sha256':hashlib.sha256((pathlib.Path(root)/'src/sapi/parallel_host.h').read_bytes()).hexdigest()}
(p.parents[1]/'build-identity.json').write_text(json.dumps(record,indent=2)+'\n')
PY
printf 'Built %s\nPHP INI: extension=%s\n' "$parallel_build/modules/parallel.so" "$parallel_build/modules/parallel.so"
