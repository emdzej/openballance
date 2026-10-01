#!/bin/bash
# Wait until emdzej/gasm publishes a release newer than $1 (default 0.3.0); print its tag and whether
# its spec/abi.json has gasm:gfx create_texture. Used while OpenBallance waits for textured gfx.
base=${1:-0.3.0}
while true; do
  tag=$(gh release list -R emdzej/gasm --limit 1 --json tagName --jq '.[0].tagName' 2>/dev/null)
  if [ -n "$tag" ] && [ "$tag" != "$base" ]; then
    tex=$(gh api "repos/emdzej/gasm/contents/spec/abi.json?ref=$tag" --jq .content 2>/dev/null | base64 -d 2>/dev/null | grep -c '"create_texture"')
    echo "gasm release $tag (create_texture in ABI: ${tex:-0})"
    exit 0
  fi
  sleep 120
done
