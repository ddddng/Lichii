# GitHub patch catalog

The GitHub catalog indexes existing patch and cheat repositories. The server
does not copy files or serve downloads. Each listing names the source and
author, links to a full Git commit, and records the license status. The desktop
downloads eligible patches directly from that commit on GitHub, verifies the
catalog SHA-256, and installs only the selected patch or cheat section.

## Reviewed sources

The upstream trees and READMEs below do not declare redistribution terms for
their patches. GitHub's license endpoint also returned 404 for all five
repositories. The importer records `NOASSERTION`, which is not a license grant.
None of the five reviewed READMEs explicitly forbids direct downloads by
other tools. Review new pins and any path-specific terms before adding them.
Keep sources that forbid third-party tools link-only. Also omit downloads for
patches whose format, target, or selected component cannot be established.
The Octopath Traveler 0 README prohibits reuploads and asks readers to use
the original links. The server does not reupload those files.

| Source | Reviewed commit | License status |
|---|---|---|
| [Kenji-NX/switch-pchtxt-mods](https://github.com/Kenji-NX/switch-pchtxt-mods/tree/13f0e6559136ead857e7c66d53f1ef1775d339f4) | `13f0e6559136ead857e7c66d53f1ef1775d339f4` | Not declared |
| [Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods](https://github.com/Fl4sh9174/Switch-Emulator-Ultrawide-FPS-Mods/tree/5eed21701e8091389e2e746f073f694ea2ad4ec6) | `5eed21701e8091389e2e746f073f694ea2ad4ec6` | Not declared |
| [StevensND/ue4-emuswitch-60fps](https://github.com/StevensND/ue4-emuswitch-60fps/tree/53c69bf93c2f79a6a0e6e7a46f26ea0f0010f06c) | `53c69bf93c2f79a6a0e6e7a46f26ea0f0010f06c` | Not declared |
| [ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats](https://github.com/ChanseyIsTheBest/NX-60FPS-RES-GFX-Cheats/tree/a2755da3eea706734d459349370757fbe2375cfc) | `a2755da3eea706734d459349370757fbe2375cfc` | Not declared |
| [StevensND/switch-port-mods](https://github.com/StevensND/switch-port-mods/tree/774e7f7c85561ddd865c1cab555f2a335054d522) | `774e7f7c85561ddd865c1cab555f2a335054d522` | Not declared |

Use the title IDs and game names declared by the source. A conflicting
README, archive path, or game mapping is not enough to establish a target.
The Chansey repository's `GAMES.md` links to both the cheat and its game
credit file. A documented 16-digit cheat build ID compares with the first
eight bytes of the installed main executable's build ID. An untyped IPS or
IPSwitch build ID does not prove a mismatch with main.

The desktop needs GitHub catalog support to show these entries. Older desktop
builds continue to see the same GameBanana and managed listings.

## Build and publish a manifest

Clone the five repositories from GitHub into one scratch directory, using
each repository's basename. The importer reads the reviewed commit from
the local Git object store. It does not watch the working tree, contact
GitHub, or execute scripts from the clones.

```sh
scratch="$HOME/.capy/work/github-catalog"
python3 -I tools/modstore-server/import_github.py "$scratch/clones" \
  --batch-dir "$scratch/batches.next" \
  > "$scratch/manifest.next.json" 2> "$scratch/import.log"
```

Review the manifest and the omission counts before publishing it. If Git
does not have a reviewed commit, fetch that exact commit rather than
substituting a branch tip. Reusing the clones makes the import repeatable
without API requests.

Deploy a server that supports the GitHub endpoint. From its Compose
directory, publish the reviewed metadata through the private admin
listener:

```sh
for batch in "$scratch"/batches.next/github-*.json; do
  sudo docker compose exec -T modstore /modstore admin \
    PUT /admin/v1/github application/json < "$batch" || break
done
```

The import keeps moderation decisions and does not remove omitted entries.
If an author removes a source or asks for removal, reject the listing through
the admin API. Verify the updated listing with the `github-v1` header and
check that requests without the header still return the legacy catalog.
