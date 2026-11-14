# PSVSend

[![XERJ Corpus Hub contributor](https://hub.xerj.org/contributors/LinuxMint-User/badge.svg)](https://hub.xerj.org/contributors/LinuxMint-User)

[简体中文](README.md) | **English**

A [LocalSend](https://localsend.org)-compatible client for the **PS Vita**. It transfers files over the local network with other LocalSend devices (phones, PCs).

> This is an independent homebrew project. It only implements the open LocalSend protocol (v2) and is not affiliated with the LocalSend brand or project.

> This project is not affiliated with or endorsed by Sony or PlayStation. It requires a jailbroken console (e.g. HENkaku / Ensō) and must be used in compliance with local laws and platform terms. Use at your own risk; intended for lawful personal use only.

> Current version **v2.7.0** (LocalSend protocol v2, compatible with official clients v1.15+). The **About** row at the top of Settings shows the version (including build tag) and compatibility info. Official VPK builds are produced by GitHub Actions and published to [Releases](https://github.com/LinuxMint-User/PSVSend/releases).

> **Project status**: Core features are complete. The project is in a stable maintenance phase: bug fixes and LocalSend compatibility updates take priority; no new features are actively planned.

## Features

- **Two-way transfers**: automatically discovers official LocalSend clients (v1.15+) on phones / PCs / tablets and exchanges files with them.
- **Encrypted transport**: supports both plain HTTP and HTTPS (mTLS device certificates); the receiver serves both on the same port. Receive 5.21 / send 4.66 MiB/s, already at the Vita NIC limit.
- **Device discovery**: UDP announce plus the peer's HTTP register. The Vita cannot receive multicast, so press **△** in the Devices pane to scan the /24 subnet; previously seen devices are probed first.
- **Device identity verification**: a **Verify** button on the send-waiting and receive-confirmation pages shows the same 128-hex code as both text and a 4×4 icon grid; only available on encrypted connections.
- **Receiving**: select files, rename each file, and choose a temporary save folder on the confirmation page; duplicate names default to a `(k)` suffix, with an overwrite option for top-level file conflicts; one failed file does not abort the rest.
- **Clipboard text**: when a peer sends copied text (the official client sends it as a single text file), it is detected automatically and opened in a read-only preview as soon as it arrives. Nothing is written to disk by default: tap **Save** or press **□** to store it as `clipboard-<time>.txt`; going back discards it.
- **Folder receiving**: manifests with relative paths recreate the original hierarchy; same-name folders merge by default (rename / skip available), top-level files can be overwritten / renamed / skipped, and nested conflicts can rename the top-level folder; each top-level folder is staged in `<name>.psvsend-part/` and renamed or merged when complete; staging folders left by a cancelled or killed transfer are cleaned up on the next start.
- **Folder sending**: when the Files pane is empty, choose **Send folder**, navigate into the target folder and press **□** to send the whole tree (enumerated in a background thread, up to 512 files) with relative paths in the manifest; empty folders are not sent, and files with over-long names or paths are skipped and reported on the home strip.
- **Checksums and concurrency**: optional sha256 before sending / on receive; 1–6 concurrent uploads with per-file failure isolation.
- **No screen-off during transfers**: while sending, receiving, or downloading from the web page, only the system's screen-off and auto-standby timers are postponed — the screen still dims to save power, but never turns off and the console never suspends mid-transfer. Idle behaviour is unchanged.
- **Web share**: a plain HTTP page on the PSV; enter a 4-digit PIN in a browser to download the selected files or upload files and/or whole folders to the PSV. The run page shows a QR code that opens the page with the PIN pre-filled; up to 4 source devices are listed.
- **QR scanning**: enter from the **L shoulder button** or the header **Scan** button, then hold **R** to scan a share QR code. If several codes are visible, the frame freezes and you pick one by position with the D-pad; the result reuses the receive confirmation page.
- **Interface and controls**: 6 languages, Yaru / OLED / custom themes × light/dark, two-pane home screen, touch + shoulder buttons; hold **R** to expand header/footer hints; idle tips rotate on the strip below the header; a bundled monochrome emoji font renders emoji in file names and clipboard text.
- **Updates and recovery**: in-app update check (manual / daily / weekly / monthly); fonts load from RAM at startup; discovery recovers after standby or Wi-Fi loss.

Detailed implementation history (build tags and design notes) is in [docs/changelog.md](docs/changelog.md) (Simplified Chinese).

## Installation

> Developed and tested on **HENkaku / Ensō 3.65**. Other firmware / jailbreak versions (3.60, 3.68, 3.73, etc.) have not been individually verified. If you hit a problem, please open an issue with your firmware and jailbreak versions.

1. Copy `psvsend.vpk` to `ux0:data/` on a jailbroken PS Vita.
2. Open it with VitaShell and install.
3. A PSVSend bubble appears on the home screen.

## Usage

> Physical buttons are the primary input; touch is an auxiliary path. The top-right header has four buttons: **Scan QR / Web Share / Scan / Settings**. The Settings, color-picker, receive-setup, and folder-picker pages have a **Back** button in the top-right. The first row of the file and folder lists is always `..` (go to parent). The bottom-right buttons on the file-browser / folder-picker / color-picker pages are **Done / Save here / Save**; the clipboard preview page has a **Save** button in the bottom-right. Tapping rows and buttons performs the same action as the corresponding button path.

### Sending

1. Open PSVSend and wait for Wi-Fi. When the device pane no longer shows the offline hint, it is ready (there is no separate status light). Once a peer receives our UDP announce it sends an HTTP register and appears in the list. If it does not appear, press **△** to scan the subnet manually.
2. The home screen has two panes (Devices + Selected Files; swap them in Settings › Layout). Press **□** to browse `ux0:/` (multi-select; press **□** again to return), then select a target in Devices and press the confirm button to send. **△** in the Files pane is **Clear all** and requires a **long press** of about 700 ms (a progress ring is drawn around the icon); this avoids confusion with the short-press scan in Devices. When the pane is empty, Up/Down switches between **Select files** and **Send folder**; the latter enters **directory mode** (folders only): ○ opens a subfolder, **□** sends the current folder directly, ✗ goes back one level.
3. Sending opens a **waiting page** (target device, file count, total size) until the peer accepts; once it starts receiving, the transfer page opens automatically with per-file progress, and you can cancel at any time. Rejection or connection failure is shown **in place** (no transfer page). If **Checksum before sending** is enabled (default), the waiting page first shows sha256 pre-scan progress (split evenly by file count, x/N on the right); about 12 s per 100 MB. If the peer requires a **receive PIN**, the system keyboard appears; enter it to continue (up to 3 attempts; a wrong PIN shows an error).
4. Multiple files upload with **3 concurrent workers** by default (Settings › Concurrent uploads, 1–6; 1 = sequential). Each file succeeds or fails independently: a failed file is marked red and the rest continue (the final state shows `Complete, N failed` and the progress bar turns to the warning color). If the **peer disappears** (cannot connect / handshake failure / stream break), the whole transfer stops immediately instead of waiting for timeouts.

### Receiving

1. When a peer sends files, PSVSend opens the receive confirmation page with the sender, platform, file list, and total size.
2. Focus starts on **Accept**: accept to let the peer start. **Setup** opens receive settings where you can uncheck files (unchecked files are skipped and omitted from the accept response). **Reject** returns 403.
3. If the incoming content collides with what is already in the save folder, you are asked how to proceed: **top-level folders** merge by default and can be renamed or skipped; **top-level files** can be overwritten, renamed or skipped (overwrite is not the default and replaces the old file only after the new one is complete and verified); a **nested folder** blocked by a same-name file renames the top-level folder by default, and you can also skip it or go back.
4. After accepting, the receive progress page shows per-file and total progress; you can cancel at any time. When the transfer finishes it briefly shows “Finalizing…” and cannot be cancelled during that phase.
5. Files are saved to `ux0:data/psvsend/downloads/` by default (Settings › Save folder changes the default; Setup can change the folder for this transfer only). Data is written as it arrives and renamed when complete. Folders are first staged in `<name>.psvsend-part/`, then renamed as a whole or merged into an existing folder; duplicate names default to a `(k)` suffix, and top-level file conflicts can be overwritten (the old file is replaced only after verification). A broken stream or checksum failure deletes partial files; stale staging folders from a previous crash are cleaned on the next start. If the peer provides sha256, it is verified by default (Settings › Verify on receive can disable it), in parallel with writing and without affecting throughput.
6. When the peer sends **clipboard text** (the official client sends copied content as a single text file), a read-only preview opens automatically when the transfer completes (scroll with the D-pad or by dragging). The text is **not written to disk** by default: tap **Save** in the bottom-right corner or press **□** to store it as `clipboard-YYYYMMDD-HHMMSS.txt`; going back discards it. If the same batch also contains regular files, it is not treated as clipboard text and is received as normal files.

### Web Share (browser transfers)

1. Three entry points: **START** on the home screen (opens sharing directly when files are selected), the **globe** button in the header, or Settings › Actions › **Web Share**. The run page shows the gateway address, a 4-digit PIN, and a QR code (scan it with a phone to open the page with the PIN pre-filled).
2. Open `http://<PSV IP>:<port>` in a browser on the same Wi-Fi and enter the PIN. In download mode the list is the selected files; tap to download. In upload mode there are two entries — **choose files** and **choose a folder** — which append to the same list and can be mixed; folders keep their relative paths (up to 2048 files per request, and the limit text follows the PS Vita setting). Selected items are listed below with per-item remove and clear-all; after a fully successful send the selection clears automatically, so you can send another batch. If the PSV is still on the previous batch's completion page, it leaves that page and shows the new request automatically. The PSV shows the receive confirmation page and the browser waits until you accept or reject.
3. To stop sharing, hold the **round button** on the run page or hold **Back** for about 0.7 s (a short press does nothing, to prevent accidents). Stopping invalidates the PIN, clears sessions, and aborts in-progress downloads / pending uploads (download managers show a failure rather than saving a truncated file).
4. Refreshing the browser requires the PIN again (the download page can reuse `sessionId` within the same tab). Three wrong PINs from the same IP trigger a 30-second cooldown, after which it recovers automatically.

### Scanning a Share QR Code

1. Two entry points: the **L shoulder button** or the **Scan** button in the top-right header (hold **R** to expand the button labels; disable the entry in Settings › Actions › QR scan).
2. Point the camera at the share-link QR code on the peer's **Web Share** page (`http://<IP>:<port>/?pin=NNNN`) and **hold R** to focus and scan. The viewfinder banner only tells you to aim; find the right distance yourself.
3. If several codes are framed at once, the image freezes; use the D-pad to pick one by position (four directions). Confirm to view the link details.
4. **Open** on the details page runs `prepare-download?pin=` in the background to fetch the file list, then shows the same **receive confirmation page** as a normal transfer: select files, rename each file, choose the save folder for this transfer. Accept to download each file with `GET download` into the save folder; you can cancel at any time.

### HTTPS / mTLS

The sender chooses plain HTTP or HTTPS based on the peer's announced `protocol`. The 2026 official LocalSend (Rust core) requires a **client certificate** (mTLS) for senders without a browser session; PSVSend ships a device identity certificate and presents it automatically, and pins the server certificate to the fingerprint announced by the peer (SHA-256 of the certificate) instead of relying on a CA chain.

The receiver serves **HTTPS and plain HTTP on the same port** (the first byte `0x16` selects TLS). **The trust model is trust-first**: encryption protects against passive sniffing, **not against an active MITM**; identity verification is a manual check and does not change the trust decision. TLS is pinned to 1.2, preferring **ChaCha20-Poly1305** and falling back to AES-GCM. Details and future plans are in [docs/design.md](docs/design.md) §9 (Simplified Chinese).

## Known Limitations

- **Network**: IPv4 / protocol v2 only; the Vita cannot receive UDP multicast, so active scanning covers only the /24 subnet (non-standard /16 or /23 networks are not fully scanned).
- **Sessions and capacity**: only one receive session at a time; up to 2048 files and a 4 MB prepare manifest per session (Settings › Receive file limit offers 6 tiers; the limit is raised temporarily for the current request when needed); sending can only browse `ux0:/`; single file ≤ 4 GB − 1, up to 512 files per send (the whole batch is rejected above that).
- **Folders**: both directions — the PS Vita can send a whole folder tree (recursively enumerated; empty folders are not sent); on receive the hierarchy is recreated from relative paths, and empty folders are not recreated (the protocol has no directory entries); folder names differing only in case are merged into one root (FAT is case-insensitive). Large flat batches (>64 files, each ≤256 KB) are written directly under the final name, so a hard kill during transfer may leave a partially written final-name file.
- **Path depth**: the PS Vita filesystem accepts at most **11 path segments** (`ux0:` counts as one); the 12th segment cannot be created. The receive confirmation page marks files that are too deep as skipped and warns; a shallower save folder leaves more levels available.
- **Clipboard text**: detected only when the manifest is a single `text/*` entry whose `preview` (decoded) length equals `size` (strict check); mixed batches of files and clipboard text are received as normal files. Text is capped at 256 KB; larger payloads are treated as regular files.
- **Emoji**: the bundled Noto Emoji is **monochrome** (libvita2d's text backend renders outline glyphs only, so colour emoji fonts cannot be used); ZWJ sequences such as 👨‍👩‍👧 are drawn as separate emoji side by side, and BMP symbol characters still use the CJK font.
- **Trust model**: trust-first; encryption protects against passive sniffing, not active MITM; identity verification is manual; fingerprint allow-lists / receive PIN are not implemented.
- **Web Share**: shares only the files selected for this session; single-range `Range` only; no directory browsing / multi-range / packaged downloads; up to 4 source devices are listed. Browser uploads accept files and/or whole folders mixed in one request (≤2048 files, manifest ≤4 MB).
- **QR scanning**: only standard black-and-white QR codes and `http://host[:port]/?pin=NNNN`; overlapping or low-brightness codes have a low success rate.

The full list is in [docs/limitations.md](docs/limitations.md) (Simplified Chinese).

## Building

Building requires the VitaSDK cross toolchain (installed at `vitasdk/` in the repo root; `env.sh` locates it automatically) and mbedTLS / libvita2d / freetype / bzip2 / libpng / libjpeg-turbo / zlib; quirc is vendored in the repo.

```bash
./build.sh          # configure + build + package; output: build/psvsend.vpk
```

Full instructions (SDK installation, vdpm dependencies, CI) are in [docs/building.md](docs/building.md) (Simplified Chinese).

## Repository Layout

| Path | Description |
|------|-------------|
| `src/` | Source (app / core / net / proto / ui) |
| `sce_sys/` | LiveArea assets (icon / background / startup image) |
| `fonts/` | Bundled Noto Sans and CJK regional fonts, plus monochrome Noto Emoji |
| `docs/` | Design, build, limitations, development history, protocol copy |
| `.github/workflows/` | CI build and draft Release publishing |

Full tree and per-file notes: [docs/design.md](docs/design.md) §5.6 (Simplified Chinese).

## Documentation

> Detailed documentation is currently in Simplified Chinese.

- [Design](docs/design.md): architecture, protocol flows, key decisions, roadmap
- [Feature and development history](docs/changelog.md): per-change records with build tags
- [Build guide](docs/building.md): VitaSDK, dependencies, build and CI
- [Limitations](docs/limitations.md): network, receiving, Web Share, QR scanning
- [PS Vita development lessons](docs/psv-app-dev-lessons.md): on-device pitfalls and general lessons

## License

[Apache License 2.0](LICENSE)

Icons are original designs and are not associated with the LocalSend brand.
