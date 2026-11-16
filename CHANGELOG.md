# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- **Desktop global hotkeys (`#12`):** Native push-to-talk, mute, and deafen global hotkeys on Windows (`RegisterHotKey` / key polling in `flutter_window.{h,cpp}`) and macOS (`MainFlutterWindow.swift`) over the `mumbleway/buttons` channel, with cross-path edge deduplication against focused `HardwareKeyboard` events in `ButtonController`.
- **Adaptive bitrate UI:** Auto voice bitrate toggle and manual bitrate slider in Settings (`setVoiceBitrate` / `setVoiceBitrateSetting`).
- **New-server username hint:** Show the automatically generated username explanation only when adding a new server (`add_server_screen.dart`), not when editing existing entries.
- **Supply-chain & toolchain enforcement:** Added `rust-toolchain.toml` (Rust `1.97.1`), `deny.toml` (`cargo-deny` licences/bans/sources/advisories checks), and Dependabot configuration (`.github/dependabot.yml`).

### Changed
- **Build & CI determinism:**
  - Updated `cargokit` (`builder.dart`) to resolve the toolchain from `rust-toolchain.toml` (`findRustToolchainChannel`, `_resolveToolchain`, covered by `toolchain_test.dart`) and invoke Cargo from `environment.manifestDir` so `.cargo/config.toml` is discovered cleanly.
  - Hardened `.github/workflows/team-build.yml` with read-only top-level `contents: read` permissions, 40-character SHA-pinned GitHub Actions, exact `rust-toolchain.toml` installation, gating `test` (matching `build.yml` plus `cargokit/build_tool` Dart analysis/tests) and `deny` (`cargo-deny`) jobs, short-path Windows builds directly in `$GITHUB_WORKSPACE`, and persistent Android debug keystore caching.
  - Gated GitHub release creation in `.github/workflows/publish.yml` on store upload step outcomes (`steps.upload.outcome == 'success'`) and cleaned up temporary App Store Connect API key files.
- **Documentation & compliance (`#9`, `#26`, `#27`, `#28`):** Aligned latency, Opus FEC/DTX, privacy, and third-party licence documentation (`docs/licences.md`, `docs/ru/licences.md`, including `sonora` and `connectivity_plus`) with the current codebase and removed stale compliance/patch-reject files (`HANDOVER.md`, `INTERNAL COMPLIANCE RECORD.docx`, `.rej` files).

### Fixed
- **Jitter buffer & audio engine (`#7`, `#8`):**
  - Added `SpeakerBuffer::pop_samples` so catch-up playback at `speed > 1.0` mixes full frames without 50 Hz dropouts.
  - Tracked `terminator_seq`, `drained_after_terminator`, and `ended_seq` in `SpeakerBuffer` so finished state resets reliably across consecutive monotonic utterances.
  - Added `Paydown::shift_with_speech` to pause delay repayment and hold look-ahead cushion during silence, removed real-time allocations from dehiss FFT processing and `render_segments`, released idle preview DSP state (`PreviewChain`) after `PREVIEW_IDLE_TIMEOUT` (5 s), and synced `third_party/cpal/PATCH.md`.
  - Fed `AutoBitrate` and `MultiLinkTracker` fresh UDP RTT/loss samples (updating `udp_pings` on `UdpEvent::Pong`), prevented upward bitrate drift during silence or packet loss, and restored worst-link bitrate selection across multi-server connections.
  - Pruned closed outgoing FFI audio senders (`prune_outgoing`, `fanout_outgoing`) and skipped diagnostics polling when no session is open.
- **Session & UDP recovery:**
  - Discarded stale queued outgoing speech after server/self unsilencing (`was_silenced`) or TCP tunnel send stalls.
  - Detected silently stale unverified UDP sockets (`is_stale_unverified`, `bound_at`, `pending_pings`), restored mid-session crypt resync throttling (`NONCE_REPLY_COOLDOWN`, `apply_crypt_setup`, `flush_pending_crypt`), and wrapped spawned reader tasks in `AbortOnDrop`.
  - Coalesced `NetworkAvailable` events (`pending_commands`, `drain_stale_network_available`), woke idle sleeps to probe TCP (`send_ping`) immediately, and expedited `udp_retry.hurry(...)` when already connected.
- **Platform & UI polish:**
  - Added wired and USB headset routing on Android, capped pre-API 31 Bluetooth SCO retries, fixed Binder thread races, and propagated `sessionRebuilt` into `AppState.onResumed`.
  - Scoped Android `MediaSession` activation to active media-button bindings (`captureMediaButtons`), enabled the macOS floating overlay (`FloatingKind.floatingWindow`, `setPhrases`/`avatars`), and added Windows single-instance window activation (`main.cpp`).
  - Bound the Settings overlay toggle to `wantOverlay`, always attached overlay callbacks (`_bindOverlayCallbacks`), and suppressed the overlay permission prompt before a first server is configured.
  - Preserved Unicode characters in exported QR filenames (`server_qr_screen.dart`) and cleaned up temporary share files.
- **Windows `cargokit` path & CMake fixes:** Redirected `CARGOKIT_TARGET_TEMP_DIR` under `%LOCALAPPDATA%\cargokit\<hash>` and propagated `CMAKE_POLICY_VERSION_MINIMUM=3.5` in `cargokit.cmake` to prevent `MAX_PATH` (`C1083`) and CMake 4+ failures in `audiopus_sys`/`libopus`.

### Removed
- **Unused TFLite/YAMNet stack (`#25`):** Removed `tflite_flutter` (`app/third_party/tflite_flutter`), `app/assets/models/yamnet.tflite`, `app/blobs/libtensorflowlite_c-win.dll`, `app/test/macos_tflite_dylib_test.dart`, and associated Gradle/analyzer exclusions.

### Security
- **Native credential storage (`#23`):** Added `SecretStore` over `mumbleway/secrets` to keep server and proxy passwords out of plaintext `SharedPreferences` and cloud/device backups using Android Keystore (`AES/GCM/NoPadding` + `noBackupFilesDir/secrets.enc`), Apple local Keychain (`kSecAttrSynchronizable = false` in `CloudStore.swift`), and Windows DPAPI (`CryptProtectData`), with automatic migration of legacy plaintext entries and backup exclusions in `backup_rules.xml` / `data_extraction_rules.xml`.
- **Protocol, crypto & session hardening:** Hardened OCB2 replay tracking (`CryptState::encrypt_iv` by value), bounded varint recursion, gated voice/plugin packet processing on completed handshakes (`self.self_session.is_some()`), added `SessionInner` / `connect_or_abort` handshake bounds, expired stale talker indicators (`expire_stale_talkers`, `last_voice_at`), hardened proxy URL parsing, keyed remote-unmute trust on certificate hashes (`Peers::cert_hashes`, `Peers::peer_key`, `remote_mute_sender_key`) instead of display names, and cleared Pocket Mode remote-unmute grants on local unmute.
