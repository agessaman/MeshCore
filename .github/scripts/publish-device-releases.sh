#!/usr/bin/env bash
set -euo pipefail

if [ ! -d out ]; then
  echo "Expected build output directory 'out' to exist."
  exit 1
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "GitHub CLI is required to publish device releases."
  exit 1
fi

repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is required}"
version="${FIRMWARE_VERSION:-${GIT_TAG_VERSION:-dev}}"
commit_hash="$(git rev-parse --short HEAD)"
release_prefix="${RELEASE_PREFIX:-firmware}"
release_name_prefix="${RELEASE_NAME_PREFIX:-Firmware}"

manifest_dir="$(mktemp -d)"
manifest_path="${manifest_dir}/firmware-manifest.json"
work_dir="$(mktemp -d)"

cleanup() {
  rm -rf "${manifest_dir}" "${work_dir}"
}
trap cleanup EXIT

slugify() {
  printf '%s' "$1" \
    | tr '[:upper:]' '[:lower:]' \
    | sed -E 's/[^a-z0-9]+/-/g; s/^-+//; s/-+$//'
}

asset_ext() {
  local file="$1"
  if [[ "$file" == *-merged.bin ]]; then
    printf 'merged.bin'
    return
  fi
  printf '%s' "${file##*.}"
}

json_escape() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\n'/\\n}"
  value="${value//$'\r'/}"
  printf '%s' "$value"
}

mapfile -t outputs < <(find out -maxdepth 1 -type f | sort)
if [ "${#outputs[@]}" -eq 0 ]; then
  echo "No firmware files found in out."
  exit 1
fi

declare -A envs=()
version_suffix="-${version}-${commit_hash}"

for file in "${outputs[@]}"; do
  base="$(basename "$file")"
  stem="${base%.*}"
  if [[ "$base" == *-merged.bin ]]; then
    stem="${base%-merged.bin}"
  fi
  env_name="${stem%${version_suffix}}"
  if [ "$env_name" = "$stem" ]; then
    echo "Skipping '${base}', it does not match expected version suffix '${version_suffix}'."
    continue
  fi
  envs["$env_name"]=1
done

printf '{\n  "version": "%s",\n  "commit": "%s",\n  "generated_at": "%s",\n  "firmwares": [\n' \
  "$(json_escape "$version")" \
  "$(json_escape "$commit_hash")" \
  "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" > "$manifest_path"

first_env=1
for env_name in $(printf '%s\n' "${!envs[@]}" | sort); do
  tag="latest-$(slugify "${release_prefix}-${env_name}")"
  release_name="${release_name_prefix}: ${env_name}"
  target_dir="${work_dir}/${env_name}"
  mkdir -p "$target_dir"

  mapfile -t env_files < <(find out -maxdepth 1 -type f -name "${env_name}-${version}-${commit_hash}*" | sort)
  if [ "${#env_files[@]}" -eq 0 ]; then
    continue
  fi

  upload_args=()
  asset_json=""
  first_asset=1
  for file in "${env_files[@]}"; do
    ext="$(asset_ext "$(basename "$file")")"
    asset_name="${env_name}.${ext}"
    stable_file="${target_dir}/${asset_name}"
    cp "$file" "$stable_file"
    upload_args+=("${stable_file}")

    download_url="https://github.com/${repo}/releases/download/${tag}/${asset_name}"
    if [ "$first_asset" -eq 0 ]; then
      asset_json="${asset_json},"
    fi
    asset_json="${asset_json}
        {
          \"name\": \"$(json_escape "$asset_name")\",
          \"type\": \"$(json_escape "$ext")\",
          \"download_url\": \"$(json_escape "$download_url")\"
        }"
    first_asset=0
  done

  body_file="${target_dir}/release-body.md"
  cat > "$body_file" <<EOF
Automated latest build for \`${env_name}\`.

- Version: \`${version}\`
- Commit: \`${commit_hash}\`
- Source workflow: [${GITHUB_WORKFLOW}](${GITHUB_SERVER_URL}/${repo}/actions/runs/${GITHUB_RUN_ID})

These assets use stable filenames so Toolbox can link to them directly.
EOF

  git tag -f "$tag" "$GITHUB_SHA"
  git push -f origin "refs/tags/${tag}"

  if gh release view "$tag" --repo "$repo" >/dev/null 2>&1; then
    gh release edit "$tag" --repo "$repo" --title "$release_name" --notes-file "$body_file" --latest=false
  else
    gh release create "$tag" --repo "$repo" --title "$release_name" --notes-file "$body_file" --target "$GITHUB_SHA" --latest=false
  fi

  gh release upload "$tag" "${upload_args[@]}" --repo "$repo" --clobber

  if [ "$first_env" -eq 0 ]; then
    printf ',\n' >> "$manifest_path"
  fi
  printf '    {\n      "id": "%s",\n      "tag": "%s",\n      "release_url": "%s",\n      "assets": [%s\n      ]\n    }' \
    "$(json_escape "$env_name")" \
    "$(json_escape "$tag")" \
    "$(json_escape "https://github.com/${repo}/releases/tag/${tag}")" \
    "$asset_json" >> "$manifest_path"
  first_env=0
done

printf '\n  ]\n}\n' >> "$manifest_path"

manifest_tag="latest-$(slugify "${release_prefix}-manifest")"
manifest_body="${manifest_dir}/manifest-body.md"
cat > "$manifest_body" <<EOF
Machine-readable firmware download manifest for Toolbox.

Use \`firmware-manifest.json\` to map device build environments to stable GitHub Release asset URLs.
EOF

git tag -f "$manifest_tag" "$GITHUB_SHA"
git push -f origin "refs/tags/${manifest_tag}"

if gh release view "$manifest_tag" --repo "$repo" >/dev/null 2>&1; then
  gh release edit "$manifest_tag" --repo "$repo" --title "${release_name_prefix} manifest" --notes-file "$manifest_body" --latest=false
else
  gh release create "$manifest_tag" --repo "$repo" --title "${release_name_prefix} manifest" --notes-file "$manifest_body" --target "$GITHUB_SHA" --latest=false
fi

gh release upload "$manifest_tag" "$manifest_path" --repo "$repo" --clobber
