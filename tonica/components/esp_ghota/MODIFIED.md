Vendored from Fishwaldo/esp_ghota at commit 335b33062b2f3a7e288586c9da34533421c690fe (MIT, see LICENSE).
Bundles lwjson (Tilen Majerle, MIT) and semver.c (Tomás Aparicio, MIT), unchanged.

Tónica uses it only to find the latest GitHub release and its assets (`ghota_check`); the install,
with its signature check, is Tónica's own (`tonica/main/auto_update.cpp`). `ghota_update`, the
storage update and the timer are left as they were and are not called.

Modifications (tonica, 2026-11-07), each marked `tonica modification` in the source:
- `ghota_check` clears the previous result first. Upstream set it only in `ghota_init`, so every
  check after the first kept the first answer and never saw a newer release.
- Chunked replies are parsed. Upstream skipped them, and api.github.com answers `releases/latest`
  chunked, so no release was ever found.
- The asset scan clears the pending name/url after every asset. Upstream did so only for an
  unmatched one, so the asset after a match could be paired with the matched one's name.
- A detached signature asset is matched too (`signaturenamematch`), and `ghota_get_assets()`
  returns the matched firmware and signature names and URLs.
- Event posts and storage calls no longer go through `ESP_ERROR_CHECK` (posts waited forever and
  aborted on failure); they log instead.
- `idf_component.yml` removed (built as a local component).
