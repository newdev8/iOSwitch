#!/bin/sh
# Compress a downloaded Metal shader seed with APFS/HFS+ transparent compression.
#
# The seed's m2v packs are mostly text-like shader records and shrink about three
# times; the raw pack is already compact. `ditto --hfsCompression` writes the
# decmpfs attribute that iOS reads transparently, and `//:image`'s merge keeps it
# (tools/install/populate.py copies the seed with ditto). The image then packs
# only the compressed blocks, so the iOS partition and the SII parts shrink too.
#
#   ./fetch-ios27.sh
#   ./compress-seed.sh
#   ./run-image.sh --ios27 --out out/sd
#
# Pass another tree as the first argument.

set -eu

cd "$(dirname "$0")"
TREE=${1:-.release-ios27/seed/tree}
[ -d "$TREE" ] || {
    echo "error: $TREE is not a directory" >&2
    exit 1
}

chmod -R u+w "$TREE"
find "$TREE" -type f -exec sh -c '
  for file do
    rm -f "$file.compressing"
    ditto --hfsCompression "$file" "$file.compressing" || exit
    mv "$file.compressing" "$file"
  done
' sh {} +

# The builder's space check takes the seed's blocks from this file, so record
# what the tree now occupies rather than what it did before compression.
chmod u+w "$TREE.size.json" 2>/dev/null || true
python3 - "$TREE" <<'PY'
import json
import os
import sys

root = sys.argv[1]
size = blocks = files = 0
for directory, _, names in os.walk(root):
    for name in names:
        stat = os.stat(os.path.join(directory, name))
        size += stat.st_size
        blocks += (stat.st_blocks * 512 + 4095) // 4096
        files += 1
with open(root + '.size.json', 'w') as out:
    json.dump({'bytes': size, 'files': files, 'blocks4k': blocks}, out)
    out.write('\n')
os.chmod(root + '.size.json', 0o644)
PY

size=$(du -sh "$TREE" | cut -f1)
echo "compressed $TREE: $size on disk"
