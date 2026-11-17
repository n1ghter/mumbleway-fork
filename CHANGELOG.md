# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Desktop global hotkeys (`#12`):** Opt-in per-binding (`global: false` by default) push-to-talk, mute, and deafen global hotkeys on Windows (`RegisterHotKey` + modifier-aware release polling in `flutter_window.{h,cpp}`) and macOS (Carbon `RegisterEventHotKey` / `kEventHotKeyPressed` & `kEventHotKeyReleased` without Accessibility permissions in `MainFlutterWindow.swift`), enforcing modifier or F-key/non-typing validation and cross-path edge deduplication in `ButtonController`.
- **Adaptive bitrate UI:** Auto voice bitrate toggle and manual bitrate slider in Settings (`setVoiceBitrate` / `setVoiceBitrateSetting`).
- **New-server username hint:** Show the automatically generated username explanation only when adding a new server (`add_server_screen.dart`), not when editing existing entries.
- **Supply-chain & toolchain enforcement:** Added `rust-toolchain.toml`, `deny.toml` (`cargo-deny` licences/bans/sources/advisories checks), and Dependabot configuration (`.github/dependabot.yml`).

### Changed
- **Build & CI determinism:**
  - Updated `cargokit` (`builder.dart`) to resolve the toolchain from `rust-toolchain.toml` (`findRustToolchainChannel`, `_resolveToolchain`, covered by `toolchain_test.dart`) and invoke Cargo from `environment.manifestDir` so `.cargo/config.toml` is discovered cleanly.
  - Hardened `.github/workflows/team-build.yml` with read-only top-level `contents: read` permissions, 40-character SHA-pinned GitHub Actions, exact `rust-toolchain.toml` installation, gating `test` (including `cargokit/build_tool` `dart test -c source`) and `deny` (`cargo-deny`) jobs, short-path Windows builds, and persistent Android debug keystore caching.
  - Updated `.github/workflows/publish.yml` to preserve build artifacts on failed store uploads (`if: always() && steps.gate.outputs.ready == 'true'`), exclude unsubmitted assets from public releases, record per-store submission status in `NOTES.md`, stage partial releases as Draft, and clean up temporary App Store Connect API keys (`umask 077`).
  - Updated `tool/push_play_listing.mjs` to count both `completed` and `inProgress` Google Play releases as live.
- **Documentation & compliance (`#9`, `#26`, `#27`, `#28`):** Aligned latency, Opus FEC/DTX, privacy, and third-party licence documentation (`docs/licences.md`, `docs/ru/licences.md`, `core/Cargo.toml` removing unused `aes-gcm`, `app/rust/Cargo.toml` adding `GPL-3.0-or-later`, adding `sonora`/WebRTC notices) and removed leftover `third_party/tract-core/*.rej` files while preserving owner compliance/handover files and optional TFLite staging (`#47` staged separately as Draft).

### Fixed
- **Jitter buffer & audio engine (`#7`, `#8`, `#40`):**
  - Added `SpeakerBuffer::pop_samples` so catch-up playback at `speed > 1.0` mixes full frames without 50 Hz dropouts, and tracked `terminator_seq`, `drained_after_terminator`, and `ended_seq` across consecutive monotonic utterances.
  - Updated `LevelNormalizer` to converge on perceived output level (`input + gain`) and track per-speaker noise floors cleanly.
  - Separated speech-gated `Paydown::shift_with_speech` (`fix/paydown-speech-gated-refill`), pre-allocated FFT scratch buffers in `SpectralSubtractor`, released idle `"mw-preview"` enhancer chains after `PREVIEW_IDLE_TIMEOUT` (5 s), fixed `render_segments` sample counts, and synced `third_party/cpal/PATCH.md` (`minSdk >= 28`).
  - Fed `AutoBitrate` and `MultiLinkTracker` fresh UDP RTT/loss samples (`udp_pings` on `UdpEvent::Pong`), prevented upward drift on silence/loss, and restored worst-link bitrate aggregation.
  - Pruned closed outgoing FFI audio senders (`prune_outgoing`, `fanout_outgoing`).
- **Session & UDP recovery:**
  - Discarded stale queued outgoing speech after unsilencing (`was_silenced`) or TCP tunnel send stalls.
  - Detected silently stale unverified UDP sockets (`is_stale_unverified`, `bound_at`), throttled mid-session crypt resyncs (`NONCE_REPLY_COOLDOWN`, `apply_crypt_setup`, `flush_pending_crypt`), and aborted leaked reader tasks via `AbortOnDrop`.
  - Coalesced `NetworkAvailable` reconnect storms (`pending_commands`, `drain_stale_network_available`), probed immediately when connected, and filtered non-routing links in `NetworkWatch`.
  - Extracted non-conflicting session abort (`connect_or_abort`), deferred initial roster emits until `ServerSync`, and expired stale talker indicators (`TALKING_TIMEOUT`).
- **Platform & UI polish:**
  - Restored Android `GET_DEVICES_OUTPUTS` (`A2DP | SCO | BLE`) detection in `AudioRouting.bluetoothHeadsetPresent()` for slow-HFP headsets alongside wired/USB/BLE routing, bounded pre-API 31 SCO retries, and Binder thread `mainHandler.post` safety (`#19`).
  - Scoped Android `MediaSession` lifecycle to active calls (`OverlayService.kt`), propagated `sessionRebuilt` on iOS interruption resumes (`AudioSession.swift`), preserved Unicode QR export filenames without deleting temp files prematurely on `ShareResultStatus.unavailable` (`#22`), and bound Settings overlay controls to `wantOverlay` with `Future.sync` fingerprint safety (`#38`).
- **Windows `cargokit` path & CMake fixes:** Redirected `CARGOKIT_TARGET_TEMP_DIR` under `%LOCALAPPDATA%\cargokit\<hash>` and propagated `CMAKE_POLICY_VERSION_MINIMUM=3.5` in `cargokit.cmake` (`#31`).

### Security
- **Native credential storage (`#23`):** Stored server and proxy passwords in `SecretStore` over `mumbleway/secrets` (Android Keystore AES-GCM in `noBackupFilesDir/secrets.enc`, Apple local Keychain with `kSecAttrSynchronizable = false`, and Windows DPAPI in `%LOCALAPPDATA%\MumbleWay\secrets.bin`), failing safely on unreadable/undecryptable stores without overwriting data or stripping fallback preferences, and preserving existing Android Auto Backup and `device-transfer` XML rules for server lists and client identities.
- **Protocol, crypto & session hardening:** Cleared OCB2 decrypt replay history on `set_decrypt_iv()`, rejected 0-byte ciphertexts without adding unsafe IV rollbacks on UDP send errors, anchored `ResyncGuard` consecutive failure tracking on first failure, iteratively rejected nested negative varints, matched UDP pongs in a bounded `MAX_PENDING_PINGS` window, preserved delimiters in proxy URL credentials, and authenticated remote-unmute grants via certificate hashes (`Peers::cert_hashes`, `Peers::peer_key`) instead of display names.
