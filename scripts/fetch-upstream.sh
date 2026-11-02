#!/bin/sh
# Fetch exact reviewed source revisions and apply the small standalone patch.
set -eu
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$project_root/upstream"
fetch_revision() {
    repository=$1
    revision=$2
    directory=$3
    if [ ! -d "$directory/.git" ]; then
        git clone --filter=blob:none "$repository" "$directory"
    fi
    current=$(git -C "$directory" rev-parse HEAD 2>/dev/null || true)
    if [ "$current" != "$revision" ]; then
        if [ -n "$(git -C "$directory" status --porcelain)" ]; then
            printf '%s\n' "Refusing to replace modified source checkout: $directory" >&2
            exit 1
        fi
        git -C "$directory" fetch --depth 1 origin "$revision"
        git -C "$directory" checkout --detach "$revision"
    fi
}
fetch_revision https://github.com/h2o/h2o.git \
    7bbb3e39bf03027344f36b4650e5383c3b6b1014 "$project_root/upstream/h2o"
fetch_revision https://github.com/php/frankenphp.git \
    4384c506d219cd2f8750edfceae57d3db8eb54f3 "$project_root/upstream/frankenphp"
fetch_revision https://github.com/php/php-src.git \
    136e049ac10b032c83018e65b63b6df858943750 "$project_root/upstream/php-src"
patch_file="$project_root/patches/h2o-native-php.patch"
if git -C "$project_root/upstream/h2o" apply --reverse --check "$patch_file" 2>/dev/null; then
    printf '%s\n' 'H2O native PHP patch already applied.'
elif git -C "$project_root/upstream/h2o" apply --check "$patch_file"; then
    git -C "$project_root/upstream/h2o" apply "$patch_file"
else
    printf '%s\n' 'H2O checkout differs from the pinned patch; review local changes.' >&2
    exit 1
fi
