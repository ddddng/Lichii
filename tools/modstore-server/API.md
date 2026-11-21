# Mod Store API, v1

## Public catalog

All paths are relative to the configured origin. JSON fields are UTF-8.
Treat text fields as plain text, not HTML or Markdown. No request needs
an account or token.

| Request | Response |
|---|---|
| `GET /healthz` | `{"status":"ok"}`, or 503 if the database or asset directory is unavailable. |
| `GET /api/v1/games` | `{"games":[{"title_id":"0400C3F00006E000","name":"Mario Kart World","mod_count":2}],"next_offset":null}`. Only games with an available mod are included. The count is of distinct mods, including demos. |
| `GET /api/v1/mods` | `{"mods":[...],"next_offset":null}`. Each result has the fields shown below, except `description` and `versions`. |
| `GET /api/v1/mods/{id}` | One mod with its approved versions. |
| `GET /api/v1/mods/{id}/preview` | The original PNG or JPEG, when present. |
| `GET /api/v1/mods/{id}/versions/{version_id}/download` | The validated ZIP, with `Content-Length` for full responses. |

The game and mod lists accept `limit` from 1 to 100, default 50.
`offset` starts at 0 and accepts values up to 1,000,000.
Continue with `next_offset` until it is `null`. Keep filters and sort the
same between pages. Lists return empty arrays when no row matches.

Games are ordered by name, with title ID as a tie breaker. The mod list
accepts these optional parameters:

| Parameter | Meaning |
|---|---|
| `title_id` | 16 hex digits, case-insensitive. Matches an approved version for that title. |
| `q` | Literal substring, up to 128 characters after trimming. Matches name, summary, description or author. SQLite folds ASCII case; it does not do locale-aware matching. |
| `type` | One of `graphics`, `fps`, `cheats`, `translation`, `gameplay`. |
| `sort` | `updated` by most recent public mod change first (default), or `name` by name. Both break ties by mod ID. |

For example, a mod with one synthetic target might look like this.
The sizes and SHA-256 below illustrate the fields; read the running API
for the generated fixture's values.

```json
{
  "id": "demo-kart-translation",
  "title_ids": ["0400C3F00006E000"],
  "name": "Language Card (demo)",
  "summary": "An inert sample for testing the Lichii Mod Store.",
  "description": "This demo has a README and a uniquely named text marker.",
  "author": "Lichii",
  "type": "translation",
  "demo": true,
  "preview_url": "/api/v1/mods/demo-kart-translation/preview",
  "updated_at": "2026-01-01T00:00:00.000Z",
  "versions": [
    {
      "id": "1.0.0-demo",
      "title_id": "0400C3F00006E000",
      "game_version": "modstore-demo",
      "build_id": "0000000000000000000000000000000000000000000000000000000000000000",
      "min_lichii_build": "build-1",
      "download_url": "/api/v1/mods/demo-kart-translation/versions/1.0.0-demo/download",
      "sha256": "b2d1143551dd5394d45c1a98d28f1bc1476bd60a6d86a37f72a37ab38dfdb7c10",
      "size_bytes": 500,
      "unpacked_size_bytes": 110
    }
  ]
}
```

`title_ids` is the sorted set of approved version title IDs, even when
the mod list is filtered by title. Versions are ordered newest upload
first, then by version ID. Each row applies to one title and one exact
version/build. A different target needs a separate version ID, even
when the archive is identical. The preview URL is nullable. Times are
UTC RFC 3339. URLs begin with `/api/v1/` and belong to the same origin.

Public records require approval of both mod and version. An unavailable
mod, version, or preview returns 404, whether absent or pending/rejected.
The API sets `Cache-Control: no-store`, including for assets. When the JSON
handler rejects a request it returns `{"error":{"code":"invalid_input","message":"..."}}`
or another stable code with the appropriate status. Unmatched paths,
methods and HTTP transport errors may return plain text instead. Handle
non-2xx status codes before parsing a success body.

## Qt compatibility and installation

Use the effective game display version and the full 32-byte build ID,
encoded as 64 uppercase hex digits, from the selected game's executable
and update. A short build ID used in a cheat filename is not enough.
Match the version's `title_id`, `game_version` and `build_id` exactly.
Do not substitute a missing or unreadable value.

Parse the decimal ordinal after `build-` as a positive unsigned 64-bit
integer. The release workflow bakes `build-<GitHub Actions run_number>`
into `Common::g_build_release_tag` for the running binary. Compare ordinals
numerically and require the running ordinal to be at least
`min_lichii_build`. Do not use lexicographic order, semantic version strings
or commit hashes. An empty tag on a local build, or an invalid tag, does
not establish compatibility.

Treat `demo: true` as ineligible for normal installation regardless of its
other fields. The sample compatibility values are synthetic and never
advertise support for a retail build. For an isolated end-to-end test,
use a test-only install path in a scratch profile and supply the matching
synthetic metadata explicitly. The demo ZIP contains a README and a uniquely
named inert RomFS text marker; it has no game content or executable code.

Check same origin before fetching media and do not follow a cross-origin
redirect. Enforce the expected download length and SHA-256, then validate
and extract the ZIP independently of the server. Entry paths are relative
to a single client-created `load/<title_id>/<mod.id>/` directory; the ZIP
does not contain that enclosing directory. `mod.id` is a portable,
lowercase slug of at most 63 characters and is the stable install directory
name. Do not overwrite an unowned mod directory or follow links in the
destination. Stage the archive and extracted files before the final
install. Reject paths outside the staging directory, links, duplicate paths
and compressed or extracted sizes over the documented limits.

## Private admin API

These routes exist only on container loopback port 8768. They require
`Authorization: Bearer <token>`; use the bundled client to avoid putting the
token in the command line. Read requests can inspect pending and rejected
records. Admin mod/version JSON adds `status`; asset URLs use the admin
prefix. Only one PUT runs at a time. Retry after a 429 once the other write
finishes.

| Request | Body or parameters |
|---|---|
| `GET /admin/v1/mods` | The public list parameters and optional `status=pending\|approved\|rejected`. |
| `GET /admin/v1/mods/{id}` | Admin detail with all versions. |
| `GET /admin/v1/mods/{id}/preview` | Original image, including when pending. |
| `GET /admin/v1/mods/{id}/versions/{version_id}/download` | Reviewed ZIP, including when pending. |
| `PUT /admin/v1/games/{title_id}` | JSON `{"name":"..."}`. |
| `PUT /admin/v1/mods/{id}` | JSON `{"name":"...","summary":"...","description":"...","author":"...","type":"fps"}`. All fields required. |
| `PUT /admin/v1/mods/{id}/preview` | Raw `image/png` or `image/jpeg`, at most 2 MiB and 2048 pixels per side. |
| `PUT /admin/v1/mods/{id}/versions/{version_id}` | Raw `application/zip`. URL-encode query fields `title_id`, `game_version`, `build_id`, `min_lichii_build`; all required. Success returns the pending version as JSON with status 201. |
| `PUT /admin/v1/mods/{id}/status` | JSON `{"status":"approved"}`; also accepts `pending` or `rejected`. |
| `PUT /admin/v1/mods/{id}/versions/{version_id}/status` | Same status body. |

Successful PUTs return 204 except for version creation. IDs and version
metadata are immutable after upload. For a correction, reject the old
version and create a new one. Version IDs are unique within a mod, at most
63 characters, and may use lowercase letters, digits, periods,
underscores and hyphens. A mod holds at most 128 versions.

The ZIP limit is 64 MiB compressed and 256 MiB extracted. The archive can
contain up to 4096 entries. It must include at least one regular file
beneath an exact lowercase `exefs/`, `romfs/`, `cheats/`, `romfs_ext/`,
or `romfslite/` root. A top-level README or LICENSE is allowed alongside
these roots. This check does not validate game data, patch semantics, or
which files the emulator will apply. Use portable ASCII names and forward
slashes. Single-disk, comment-free ZIPs with stored or deflated entries
are supported; ZIP64 central directories are not. An upload is repacked
without its original ZIP metadata. The server accepts at most 24 KiB
for JSON requests.
