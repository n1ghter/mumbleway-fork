#include "flutter_window.h"

#include <flutter/standard_message_codec.h>
#include <flutter/standard_method_codec.h>
#include <windows.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "flutter/generated_plugin_registrant.h"

namespace {

constexpr UINT_PTR kHotkeyReleaseTimerId = 1;
constexpr UINT kHotkeyReleasePollMs = 20;

constexpr int kModCtrl = 1;
constexpr int kModAlt = 2;
constexpr int kModShift = 4;
constexpr int kModMeta = 8;
constexpr int kModMask = kModCtrl | kModAlt | kModShift | kModMeta;

bool IsModifierKeyId(int64_t key_id) {
  switch (key_id) {
    case 0x00200000100LL:  // ControlLeft
    case 0x00200000101LL:  // ControlRight
    case 0x00200000102LL:  // ShiftLeft
    case 0x00200000103LL:  // ShiftRight
    case 0x00200000104LL:  // AltLeft
    case 0x00200000105LL:  // AltRight
    case 0x00200000106LL:  // MetaLeft
    case 0x00200000107LL:  // MetaRight
    case 0x20100000100LL:  // Control synonym
    case 0x20100000102LL:  // Shift synonym
    case 0x20100000104LL:  // Alt synonym
    case 0x20100000106LL:  // Meta synonym
    case 0x20200000100LL:  // Control synonym (legacy/variant)
    case 0x20200000102LL:
    case 0x20200000104LL:
    case 0x20200000106LL:
      return true;
    default:
      return false;
  }
}

bool IsSafeGlobalKey(int64_t key_id, int modifiers) {
  if (IsModifierKeyId(key_id)) {
    return false;
  }
  if ((modifiers & kModMask) != 0) {
    return true;
  }
  // Function keys F1..F24.
  if (key_id >= 0x00100000801LL && key_id <= 0x00100000818LL) {
    return true;
  }
  // Non-typing system/lock keys: PrintScreen, Pause, ScrollLock.
  return key_id == 0x00100000608LL ||
         key_id == 0x00100000509LL ||
         key_id == 0x0010000010cLL;
}

UINT ModifiersToWin32(int modifiers) {
  UINT win_mods = 0;
  if ((modifiers & kModCtrl) != 0) {
    win_mods |= MOD_CONTROL;
  }
  if ((modifiers & kModAlt) != 0) {
    win_mods |= MOD_ALT;
  }
  if ((modifiers & kModShift) != 0) {
    win_mods |= MOD_SHIFT;
  }
  if ((modifiers & kModMeta) != 0) {
    win_mods |= MOD_WIN;
  }
  return win_mods;
}

bool AreModifiersDown(UINT win_mods) {
  if ((win_mods & MOD_CONTROL) != 0 &&
      (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) {
    return false;
  }
  if ((win_mods & MOD_ALT) != 0 &&
      (GetAsyncKeyState(VK_MENU) & 0x8000) == 0) {
    return false;
  }
  if ((win_mods & MOD_SHIFT) != 0 &&
      (GetAsyncKeyState(VK_SHIFT) & 0x8000) == 0) {
    return false;
  }
  if ((win_mods & MOD_WIN) != 0 &&
      (GetAsyncKeyState(VK_LWIN) & 0x8000) == 0 &&
      (GetAsyncKeyState(VK_RWIN) & 0x8000) == 0) {
    return false;
  }
  return true;
}

constexpr size_t kMaxSecretsFileBytes = 16 * 1024 * 1024;

std::wstring SecretsFilePath(std::wstring* out_dir) {
  std::wstring base;
  PWSTR raw_path = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr,
                                     &raw_path)) &&
      raw_path != nullptr) {
    base = raw_path;
    CoTaskMemFree(raw_path);
  } else {
    if (raw_path != nullptr) {
      CoTaskMemFree(raw_path);
    }
    const DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed > 1) {
      std::wstring buf(needed, L'\0');
      const DWORD len =
          GetEnvironmentVariableW(L"LOCALAPPDATA", buf.data(), needed);
      if (len > 0 && len < needed) {
        buf.resize(len);
        base = std::move(buf);
      }
    }
  }
  if (base.empty()) {
    return std::wstring();
  }
  std::wstring dir = base + L"\\MumbleWay";
  if (out_dir != nullptr) {
    *out_dir = dir;
  }
  return dir + L"\\secrets.bin";
}

struct LocalSecretsReadResult {
  bool ok = false;
  bool unreadable = false;
  flutter::EncodableMap map;
  std::string error;
};

LocalSecretsReadResult UnreadableSecrets(std::string error) {
  LocalSecretsReadResult res;
  res.ok = false;
  res.unreadable = true;
  res.error = std::move(error);
  return res;
}

LocalSecretsReadResult ReadLocalSecrets() {
  const std::wstring path = SecretsFilePath(nullptr);
  if (path.empty()) {
    return UnreadableSecrets("Could not resolve local secrets path.");
  }

  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
      LocalSecretsReadResult res;
      res.ok = true;
      res.unreadable = false;
      return res;
    }
    return UnreadableSecrets("Failed to open local secrets file (error " +
                             std::to_string(err) + ").");
  }

  LARGE_INTEGER file_size{};
  if (!GetFileSizeEx(file, &file_size)) {
    const DWORD err = GetLastError();
    CloseHandle(file);
    return UnreadableSecrets(
        "Failed to query local secrets file size (error " +
        std::to_string(err) + ").");
  }
  if (file_size.QuadPart <= 0 ||
      static_cast<uint64_t>(file_size.QuadPart) > kMaxSecretsFileBytes) {
    CloseHandle(file);
    return UnreadableSecrets(
        "Local secrets file is empty or exceeds maximum size.");
  }

  std::vector<uint8_t> ciphertext(static_cast<size_t>(file_size.QuadPart));
  size_t offset = 0;
  while (offset < ciphertext.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (std::min)(ciphertext.size() - offset,
                   static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
    DWORD read = 0;
    if (!ReadFile(file, ciphertext.data() + offset, chunk, &read, nullptr)) {
      const DWORD err = GetLastError();
      CloseHandle(file);
      return UnreadableSecrets("Failed to read local secrets file (error " +
                               std::to_string(err) + ").");
    }
    if (read == 0) {
      break;
    }
    offset += read;
  }
  CloseHandle(file);
  if (offset != ciphertext.size()) {
    return UnreadableSecrets("Unexpected EOF while reading local secrets.");
  }

  DATA_BLOB encrypted{};
  encrypted.pbData = ciphertext.data();
  encrypted.cbData = static_cast<DWORD>(ciphertext.size());
  DATA_BLOB plaintext{};
  if (!CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &plaintext)) {
    const DWORD err = GetLastError();
    return UnreadableSecrets(
        "Failed to decrypt local secrets with DPAPI (error " +
        std::to_string(err) + ").");
  }

  if (plaintext.pbData == nullptr || plaintext.cbData == 0) {
    if (plaintext.pbData != nullptr) {
      LocalFree(plaintext.pbData);
    }
    return UnreadableSecrets("Decrypted local secrets payload is empty.");
  }

  std::vector<uint8_t> plain_bytes(plaintext.pbData,
                                   plaintext.pbData + plaintext.cbData);
  SecureZeroMemory(plaintext.pbData, plaintext.cbData);
  LocalFree(plaintext.pbData);

  std::unique_ptr<flutter::EncodableValue> decoded =
      flutter::StandardMessageCodec::GetInstance().DecodeMessage(
          plain_bytes.data(), plain_bytes.size());
  SecureZeroMemory(plain_bytes.data(), plain_bytes.size());
  if (decoded == nullptr) {
    return UnreadableSecrets("Failed to decode local secrets payload.");
  }

  const auto* raw_map = std::get_if<flutter::EncodableMap>(decoded.get());
  if (raw_map == nullptr) {
    return UnreadableSecrets("Local secrets payload is not a map.");
  }

  flutter::EncodableMap out;
  for (const auto& entry : *raw_map) {
    const auto* k = std::get_if<std::string>(&entry.first);
    const auto* v = std::get_if<std::string>(&entry.second);
    if (k != nullptr && v != nullptr) {
      out.emplace(flutter::EncodableValue(*k), flutter::EncodableValue(*v));
    }
  }
  LocalSecretsReadResult res;
  res.ok = true;
  res.unreadable = false;
  res.map = std::move(out);
  return res;
}

bool WriteLocalSecrets(const flutter::EncodableMap& secrets) {
  std::wstring dir;
  const std::wstring path = SecretsFilePath(&dir);
  if (path.empty() || dir.empty()) {
    return false;
  }

  if (secrets.empty()) {
    if (!DeleteFileW(path.c_str())) {
      const DWORD err = GetLastError();
      return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
    }
    return true;
  }

  std::unique_ptr<std::vector<uint8_t>> encoded =
      flutter::StandardMessageCodec::GetInstance().EncodeMessage(
          flutter::EncodableValue(secrets));
  if (encoded == nullptr || encoded->empty() ||
      encoded->size() > (std::numeric_limits<DWORD>::max)()) {
    return false;
  }

  DATA_BLOB input{};
  input.pbData = encoded->data();
  input.cbData = static_cast<DWORD>(encoded->size());
  DATA_BLOB protected_blob{};
  const BOOL ok = CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr,
                                   CRYPTPROTECT_UI_FORBIDDEN, &protected_blob);
  SecureZeroMemory(encoded->data(), encoded->size());
  if (!ok) {
    return false;
  }

  const int dir_status = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
  if (dir_status != ERROR_SUCCESS && dir_status != ERROR_ALREADY_EXISTS &&
      dir_status != ERROR_FILE_EXISTS) {
    if (protected_blob.pbData != nullptr) {
      LocalFree(protected_blob.pbData);
    }
    return false;
  }

  const std::wstring tmp_path = path + L".tmp";
  HANDLE file = CreateFileW(tmp_path.c_str(), GENERIC_WRITE, 0, nullptr,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (protected_blob.pbData != nullptr) {
      LocalFree(protected_blob.pbData);
    }
    return false;
  }

  size_t offset = 0;
  while (offset < protected_blob.cbData) {
    const DWORD chunk = static_cast<DWORD>(
        static_cast<size_t>(protected_blob.cbData) - offset);
    DWORD written = 0;
    if (!WriteFile(file, protected_blob.pbData + offset, chunk, &written,
                   nullptr) ||
        written == 0) {
      CloseHandle(file);
      DeleteFileW(tmp_path.c_str());
      LocalFree(protected_blob.pbData);
      return false;
    }
    offset += written;
  }
  FlushFileBuffers(file);
  CloseHandle(file);
  LocalFree(protected_blob.pbData);

  if (!MoveFileExW(tmp_path.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp_path.c_str());
    return false;
  }
  return true;
}

// Maps a Flutter `LogicalKeyboardKey.keyId` to a Windows virtual-key code for
// `RegisterHotKey`. Returns std::nullopt when no single-key VK mapping exists.
std::optional<UINT> FlutterKeyIdToVirtualKey(int64_t key_id) {
  // Printable ASCII letters a..z / A..Z map to VK_A..VK_Z (0x41..0x5A).
  if (key_id >= 0x61LL && key_id <= 0x7aLL) {
    return static_cast<UINT>('A' + (key_id - 0x61LL));
  }
  if (key_id >= 0x41LL && key_id <= 0x5aLL) {
    return static_cast<UINT>(key_id);
  }
  // ASCII digits 0..9 match their virtual-key codes.
  if (key_id >= 0x30LL && key_id <= 0x39LL) {
    return static_cast<UINT>(key_id);
  }
  // Function keys F1..F24 (0x00100000801..0x00100000818).
  if (key_id >= 0x00100000801LL && key_id <= 0x00100000818LL) {
    return static_cast<UINT>(VK_F1 + (key_id - 0x00100000801LL));
  }
  // Numpad digits 0..9 (0x00200000230..0x00200000239).
  if (key_id >= 0x00200000230LL && key_id <= 0x00200000239LL) {
    return static_cast<UINT>(VK_NUMPAD0 + (key_id - 0x00200000230LL));
  }

  switch (key_id) {
    case 0x00000000020LL:  // Space
      return VK_SPACE;
    case 0x00100000009LL:  // Tab
      return VK_TAB;
    case 0x0010000000dLL:  // Enter
    case 0x0020000020dLL:  // NumpadEnter
      return VK_RETURN;
    case 0x0010000001bLL:  // Escape
      return VK_ESCAPE;
    case 0x00100000008LL:  // Backspace
      return VK_BACK;
    case 0x0010000007fLL:  // Delete
      return VK_DELETE;
    case 0x00100000407LL:  // Insert
      return VK_INSERT;
    case 0x00100000306LL:  // Home
      return VK_HOME;
    case 0x00100000305LL:  // End
      return VK_END;
    case 0x00100000308LL:  // PageUp
      return VK_PRIOR;
    case 0x00100000307LL:  // PageDown
      return VK_NEXT;
    case 0x00100000302LL:  // ArrowLeft
      return VK_LEFT;
    case 0x00100000304LL:  // ArrowUp
      return VK_UP;
    case 0x00100000303LL:  // ArrowRight
      return VK_RIGHT;
    case 0x00100000301LL:  // ArrowDown
      return VK_DOWN;
    case 0x00100000509LL:  // Pause
      return VK_PAUSE;
    case 0x0010000010cLL:  // ScrollLock
      return VK_SCROLL;
    case 0x00100000608LL:  // PrintScreen
      return VK_SNAPSHOT;
    case 0x00100000104LL:  // CapsLock
      return VK_CAPITAL;
    case 0x0010000010aLL:  // NumLock
      return VK_NUMLOCK;
    case 0x00100000505LL:  // ContextMenu
      return VK_APPS;
    case 0x00100000401LL:  // Clear
      return VK_CLEAR;
    case 0x00100000508LL:  // Help
      return VK_HELP;
    case 0x00200000002LL:  // Sleep
      return VK_SLEEP;

    // Numpad operators
    case 0x0020000022aLL:  // NumpadMultiply
      return VK_MULTIPLY;
    case 0x0020000022bLL:  // NumpadAdd
      return VK_ADD;
    case 0x0020000022cLL:  // NumpadComma
      return VK_SEPARATOR;
    case 0x0020000022dLL:  // NumpadSubtract
      return VK_SUBTRACT;
    case 0x0020000022eLL:  // NumpadDecimal
      return VK_DECIMAL;
    case 0x0020000022fLL:  // NumpadDivide
      return VK_DIVIDE;

    // US OEM punctuation keys
    case 0x0000000003bLL:  // Semicolon
      return VK_OEM_1;
    case 0x0000000003dLL:  // Equal
      return VK_OEM_PLUS;
    case 0x0000000002cLL:  // Comma
      return VK_OEM_COMMA;
    case 0x0000000002dLL:  // Minus
      return VK_OEM_MINUS;
    case 0x0000000002eLL:  // Period
      return VK_OEM_PERIOD;
    case 0x0000000002fLL:  // Slash
      return VK_OEM_2;
    case 0x00000000060LL:  // Backquote
      return VK_OEM_3;
    case 0x0000000005bLL:  // BracketLeft
      return VK_OEM_4;
    case 0x0000000005cLL:  // Backslash
      return VK_OEM_5;
    case 0x0000000005dLL:  // BracketRight
      return VK_OEM_6;
    case 0x00000000022LL:  // Quote
    case 0x00000000027LL:  // QuoteSingle
      return VK_OEM_7;

    // Hardware media / browser keys
    case 0x00100000a05LL:  // MediaPlayPause
      return VK_MEDIA_PLAY_PAUSE;
    case 0x00100000a07LL:  // MediaStop
      return VK_MEDIA_STOP;
    case 0x00100000a08LL:  // MediaTrackNext
      return VK_MEDIA_NEXT_TRACK;
    case 0x00100000a09LL:  // MediaTrackPrevious
      return VK_MEDIA_PREV_TRACK;
    case 0x00100000a11LL:  // AudioVolumeMute
      return VK_VOLUME_MUTE;
    case 0x00100000a0fLL:  // AudioVolumeDown
      return VK_VOLUME_DOWN;
    case 0x00100000a10LL:  // AudioVolumeUp
      return VK_VOLUME_UP;
    case 0x00100000c01LL:  // BrowserBack
      return VK_BROWSER_BACK;
    case 0x00100000c03LL:  // BrowserForward
      return VK_BROWSER_FORWARD;
    case 0x00100000c05LL:  // BrowserRefresh
      return VK_BROWSER_REFRESH;
    case 0x00100000c07LL:  // BrowserStop
      return VK_BROWSER_STOP;
    case 0x00100000c06LL:  // BrowserSearch
      return VK_BROWSER_SEARCH;
    case 0x00100000c02LL:  // BrowserFavorites
      return VK_BROWSER_FAVORITES;
    case 0x00100000c04LL:  // BrowserHome
      return VK_BROWSER_HOME;
    case 0x00100000b03LL:  // LaunchMail
      return VK_LAUNCH_MAIL;

    default:
      break;
  }

  // Fallback for non-ASCII BMP keys (e.g. non-Latin layouts) that map to a
  // single unshifted virtual key on the current keyboard layout.
  if (key_id > 0x7eLL && key_id <= 0xffffLL) {
    const SHORT scan = VkKeyScanW(static_cast<WCHAR>(key_id));
    if (scan != -1 && HIBYTE(scan) == 0 && LOBYTE(scan) != 0) {
      return static_cast<UINT>(LOBYTE(scan));
    }
  }

  return std::nullopt;
}

void HandleSecretsMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (call.method_name() == "readAll") {
    LocalSecretsReadResult read_res = ReadLocalSecrets();
    if (!read_res.ok) {
      result->Error("dpapi-unreadable", read_res.error);
      return;
    }
    result->Success(flutter::EncodableValue(read_res.map));
    return;
  }
  if (call.method_name() == "writeAll") {
    const auto* incoming =
        std::get_if<flutter::EncodableMap>(call.arguments());
    if (incoming == nullptr) {
      result->Error("bad-args", "writeAll expects a string map.");
      return;
    }
    LocalSecretsReadResult read_res = ReadLocalSecrets();
    if (!read_res.ok) {
      result->Error("dpapi-unreadable", read_res.error);
      return;
    }
    flutter::EncodableMap merged = std::move(read_res.map);
    bool updated = false;
    for (const auto& entry : *incoming) {
      const auto* k = std::get_if<std::string>(&entry.first);
      const auto* v = std::get_if<std::string>(&entry.second);
      if (k != nullptr && v != nullptr) {
        merged[flutter::EncodableValue(*k)] = flutter::EncodableValue(*v);
        updated = true;
      }
    }
    if (updated && !WriteLocalSecrets(merged)) {
      result->Error("dpapi", "Failed to store encrypted secrets.");
      return;
    }
    result->Success();
    return;
  }
  if (call.method_name() == "deleteKeys") {
    const auto* keys = std::get_if<flutter::EncodableList>(call.arguments());
    if (keys == nullptr) {
      result->Error("bad-args", "deleteKeys expects a key list.");
      return;
    }
    LocalSecretsReadResult read_res = ReadLocalSecrets();
    if (!read_res.ok) {
      result->Error("dpapi-unreadable", read_res.error);
      return;
    }
    flutter::EncodableMap merged = std::move(read_res.map);
    bool changed = false;
    for (const auto& item : *keys) {
      const auto* k = std::get_if<std::string>(&item);
      if (k != nullptr && merged.erase(flutter::EncodableValue(*k)) > 0) {
        changed = true;
      }
    }
    if (changed && !WriteLocalSecrets(merged)) {
      result->Error("dpapi", "Failed to update encrypted secrets.");
      return;
    }
    result->Success();
    return;
  }
  result->NotImplemented();
}

}  // namespace

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  // Whether there is a conversation worth keeping the machine awake for.
  //
  // Its own channel rather than the overlay's: this platform has no floating
  // window and never will — on a desktop the app is a window among windows,
  // already a click away — but it sleeps like every other machine, and a call
  // that dies because a laptop lid was left open is the same failure a rider
  // gets on a phone in a pocket.
  power_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "mumbleway/power",
          &flutter::StandardMethodCodec::GetInstance());
  power_channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) {
        if (call.method_name() == "callActive") {
          const auto* active = std::get_if<bool>(call.arguments());
          SetCallActive(active != nullptr && *active);
          result->Success(flutter::EncodableValue(true));
        } else {
          result->NotImplemented();
        }
      });

<<<<<<< HEAD
  buttons_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "mumbleway/buttons",
          &flutter::StandardMethodCodec::GetInstance());
  buttons_channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) {
        if (call.method_name() == "setGlobalHotkeys") {
          const auto* args =
              std::get_if<flutter::EncodableMap>(call.arguments());
          if (args == nullptr) {
            result->Error("invalid_args", "Expected a map");
            return;
          }
          std::vector<HotkeySpec> hotkeys;
          const auto hotkeys_it =
              args->find(flutter::EncodableValue("hotkeys"));
          if (hotkeys_it != args->end()) {
            const auto* list =
                std::get_if<flutter::EncodableList>(&hotkeys_it->second);
            if (list != nullptr) {
              hotkeys.reserve(list->size());
              for (const auto& item : *list) {
                const auto* entry = std::get_if<flutter::EncodableMap>(&item);
                if (entry == nullptr) {
                  continue;
                }
                const auto key_it =
                    entry->find(flutter::EncodableValue("keyId"));
                if (key_it == entry->end()) {
                  continue;
                }
                int64_t key_id = 0;
                if (const auto* v32 = std::get_if<int32_t>(&key_it->second)) {
                  key_id = static_cast<int64_t>(*v32);
                } else if (const auto* v64 =
                               std::get_if<int64_t>(&key_it->second)) {
                  key_id = *v64;
                } else {
                  continue;
                }
                int modifiers = 0;
                const auto mod_it =
                    entry->find(flutter::EncodableValue("modifiers"));
                if (mod_it != entry->end()) {
                  if (const auto* m32 =
                          std::get_if<int32_t>(&mod_it->second)) {
                    modifiers = *m32;
                  } else if (const auto* m64 =
                                 std::get_if<int64_t>(&mod_it->second)) {
                    modifiers = static_cast<int>(*m64);
                  } else {
                    continue;
                  }
                }
                modifiers &= kModMask;
                if (!IsSafeGlobalKey(key_id, modifiers)) {
                  continue;
                }
                hotkeys.push_back(HotkeySpec{key_id, modifiers});
              }
            }
          }
          SetGlobalHotkeys(hotkeys);
          result->Success(flutter::EncodableValue(true));
        } else {
          result->NotImplemented();
        }
      });

  // Device-local DPAPI-encrypted secret storage under %LOCALAPPDATA%\MumbleWay.
  secrets_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "mumbleway/secrets",
          &flutter::StandardMethodCodec::GetInstance());
  secrets_channel_->SetMethodCallHandler(
      [](const flutter::MethodCall<flutter::EncodableValue>& call,
         std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
             result) {
        HandleSecretsMethodCall(call, std::move(result));
      });

  // Establishes the idle state rather than waiting for the first call to end.
  // The app starts with nothing connected, which is exactly the state that
  // should cost nothing.
  ApplyPowerThrottling();

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  UnregisterGlobalHotkeys();

  if (buttons_channel_) {
    buttons_channel_->SetMethodCallHandler(nullptr);
    buttons_channel_.reset();
  }
  if (power_channel_) {
    power_channel_->SetMethodCallHandler(nullptr);
    power_channel_.reset();
  }

  if (secrets_channel_) {
    secrets_channel_->SetMethodCallHandler(nullptr);
    secrets_channel_.reset();
  }

  // Given back explicitly. A process that exits holding ES_SYSTEM_REQUIRED
  // has its request dropped by the system anyway, but relying on that means
  // the one path where it is not — a window closed while the process lives on
  // — leaves a machine that will not sleep and nothing on screen to explain
  // why.
  SetCallActive(false);

  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

void FlutterWindow::SetGlobalHotkeys(const std::vector<HotkeySpec>& hotkeys) {
  const std::vector<RegisteredHotkey> previously_held = held_hotkeys_;
  UnregisterGlobalHotkeys();

  HWND hwnd = GetHandle();
  if (hwnd == nullptr) {
    for (const auto& held : previously_held) {
      InvokeGlobalHotkey(held.key_id, held.modifiers, false);
    }
    return;
  }

  std::unordered_set<uint64_t> registered_combos;
  int next_id = 1;
  for (const auto& spec : hotkeys) {
    if (next_id > 0xBFFF) {
      break;
    }
    const int modifiers = spec.modifiers & kModMask;
    if (!IsSafeGlobalKey(spec.key_id, modifiers)) {
      continue;
    }
    const std::optional<UINT> vk = FlutterKeyIdToVirtualKey(spec.key_id);
    if (!vk.has_value()) {
      continue;
    }
    const UINT win_mods = ModifiersToWin32(modifiers);
    const uint64_t combo =
        (static_cast<uint64_t>(win_mods) << 32) | static_cast<uint64_t>(*vk);
    if (!registered_combos.insert(combo).second) {
      continue;
    }
    if (RegisterHotKey(hwnd, next_id, MOD_NOREPEAT | win_mods, *vk)) {
      registered_hotkeys_.emplace(
          next_id,
          RegisteredHotkey{next_id, *vk, win_mods, spec.key_id, modifiers});
      ++next_id;
    }
  }

  for (const auto& held : previously_held) {
    const RegisteredHotkey* matching = nullptr;
    for (const auto& [id, candidate] : registered_hotkeys_) {
      (void)id;
      if (candidate.key_id == held.key_id &&
          candidate.modifiers == held.modifiers) {
        matching = &candidate;
        break;
      }
    }
    if (matching != nullptr &&
        (GetAsyncKeyState(static_cast<int>(matching->vk)) & 0x8000) != 0 &&
        AreModifiersDown(matching->win_mods)) {
      held_hotkeys_.push_back(*matching);
    } else {
      InvokeGlobalHotkey(held.key_id, held.modifiers, false);
    }
  }

  if (!held_hotkeys_.empty() && !hotkey_timer_active_) {
    if (SetTimer(hwnd, kHotkeyReleaseTimerId, kHotkeyReleasePollMs, nullptr) !=
        0) {
      hotkey_timer_active_ = true;
    }
  }
}

void FlutterWindow::UnregisterGlobalHotkeys() {
  HWND hwnd = GetHandle();
  if (hotkey_timer_active_) {
    if (hwnd != nullptr) {
      KillTimer(hwnd, kHotkeyReleaseTimerId);
    }
    hotkey_timer_active_ = false;
  }
  held_hotkeys_.clear();
  if (hwnd != nullptr) {
    for (const auto& [id, hotkey] : registered_hotkeys_) {
      (void)hotkey;
      UnregisterHotKey(hwnd, id);
    }
  }
  registered_hotkeys_.clear();
}

void FlutterWindow::InvokeGlobalHotkey(int64_t key_id,
                                       int modifiers,
                                       bool pressed) {
  if (!buttons_channel_) {
    return;
  }
  flutter::EncodableMap args;
  args[flutter::EncodableValue("keyId")] = flutter::EncodableValue(key_id);
  args[flutter::EncodableValue("modifiers")] =
      flutter::EncodableValue(modifiers);
  args[flutter::EncodableValue("pressed")] = flutter::EncodableValue(pressed);
  buttons_channel_->InvokeMethod(
      "globalHotkey",
      std::make_unique<flutter::EncodableValue>(std::move(args)));
}

void FlutterWindow::PollHotkeyReleases() {
  std::vector<HotkeySpec> released_hotkeys;
  auto it = held_hotkeys_.begin();
  while (it != held_hotkeys_.end()) {
    const SHORT state = GetAsyncKeyState(static_cast<int>(it->vk));
    if ((state & 0x8000) == 0 || !AreModifiersDown(it->win_mods)) {
      released_hotkeys.push_back(HotkeySpec{it->key_id, it->modifiers});
      it = held_hotkeys_.erase(it);
    } else {
      ++it;
    }
  }
  if (held_hotkeys_.empty() && hotkey_timer_active_) {
    if (HWND hwnd = GetHandle()) {
      KillTimer(hwnd, kHotkeyReleaseTimerId);
    }
    hotkey_timer_active_ = false;
  }
  for (const auto& released : released_hotkeys) {
    InvokeGlobalHotkey(released.key_id, released.modifiers, false);
  }
}

void FlutterWindow::SetCallActive(bool active) {
  call_active_ = active;

  // ES_SYSTEM_REQUIRED holds off the idle sleep timer for as long as it keeps
  // being asserted; ES_CONTINUOUS is what makes it stick rather than count as
  // a single nudge. Clearing it is the same call without the flag.
  //
  // Deliberately no ES_DISPLAY_REQUIRED. Keeping a machine awake is not the
  // same as keeping a screen lit, and a voice call has nothing anybody needs
  // to look at — a monitor held on for the length of a conversation would
  // cost more than everything else in this pass put together.
  //
  // This is per-thread state, and the method channel handler runs on the
  // platform thread, which is the one that lives as long as the window does.
  SetThreadExecutionState(active ? (ES_CONTINUOUS | ES_SYSTEM_REQUIRED)
                                 : ES_CONTINUOUS);

  ApplyPowerThrottling();
}

void FlutterWindow::ApplyPowerThrottling() {
  // EcoQoS: asks Windows to schedule this process for efficiency rather than
  // speed, which on a hybrid CPU means the small cores and a lower clock.
  //
  // Only while there is no call *and* nobody is looking. Either condition
  // alone would be wrong. Throttling during a call would put the audio thread
  // on an efficiency core, and a DSP chain that misses its deadline is heard
  // rather than measured. Throttling a window somebody is working in trades
  // battery for a sluggish interface, which is not the bargain this pass is
  // making anywhere else.
  //
  // An unattended window with nothing connected is neither of those. It is
  // also the state the app spends most of its life in.
  const bool efficient = !call_active_ && !foreground_;

  PROCESS_POWER_THROTTLING_STATE state = {};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask = efficient ? PROCESS_POWER_THROTTLING_EXECUTION_SPEED : 0;

  // Unchecked on purpose: this is a request, and Windows releases before
  // 1709 simply decline it. Nothing downstream depends on the answer, and a
  // machine that will not run us slowly is not a fault worth reporting.
  SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state,
                        sizeof(state));
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;

    // WM_ACTIVATEAPP rather than WM_ACTIVATE: the question is whether this
    // application is the one being used, not which of its windows has the
    // caret. Clicking between our own windows must not toggle the process
    // between efficiency and performance.
    case WM_ACTIVATEAPP:
      foreground_ = wparam != FALSE;
      ApplyPowerThrottling();
      break;

    case WM_HOTKEY: {
      const int id = static_cast<int>(wparam);
      const auto it = registered_hotkeys_.find(id);
      if (it != registered_hotkeys_.end()) {
        const RegisteredHotkey hotkey = it->second;
        InvokeGlobalHotkey(hotkey.key_id, hotkey.modifiers, true);
        // RegisterHotKey emits no key-up message, so poll the async key state
        // only while a hotkey is down so push-to-talk still releases cleanly
        // (toggle actions ignore pressed=false).
        const bool already_held = std::any_of(
            held_hotkeys_.begin(), held_hotkeys_.end(),
            [id](const RegisteredHotkey& item) { return item.id == id; });
        if (!already_held) {
          held_hotkeys_.push_back(hotkey);
        }
        if (!hotkey_timer_active_ && hwnd != nullptr) {
          if (SetTimer(hwnd, kHotkeyReleaseTimerId, kHotkeyReleasePollMs,
                       nullptr) != 0) {
            hotkey_timer_active_ = true;
          }
        }
        return 0;
      }
      break;
    }

    case WM_TIMER:
      if (wparam == kHotkeyReleaseTimerId) {
        PollHotkeyReleases();
        return 0;
      }
      break;

    case WM_DESTROY:
      // Unregister before Win32Window::MessageHandler clears window_handle_.
      UnregisterGlobalHotkeys();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
