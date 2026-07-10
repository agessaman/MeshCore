# Mesh America provider catalogs

Generated provider catalogs for the Keymind Cascade MeshCore release assets.

Send Mesh America these catalog URLs after this directory is committed and pushed to `keymindCascade`:

```text
Provider name: Keymind Cascade
Catalog URL:   https://raw.githubusercontent.com/mikecarper/MeshCore/keymindCascade/mesh-america/keymind-cascade-v1.16.0-provider.json
```

```text
Provider name: Keymind Cascade Logging
Catalog URL:   https://raw.githubusercontent.com/mikecarper/MeshCore/keymindCascade/mesh-america/keymind-cascade-logging-v1.16.0-provider.json
```

Regenerate both catalogs from the release asset folders with:

```powershell
powershell -ExecutionPolicy Bypass -File mesh-america\generate-mesh-america-catalogs.ps1
```
