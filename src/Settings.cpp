#include "Settings.h"
#include "WinUtil.h"
#include <optional>

namespace paper {
namespace {

constexpr wchar_t SettingsKey[] = L"Software\\PaperShade";
constexpr wchar_t StartupKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

class Key {
public:
    ~Key() { if (value) RegCloseKey(value); }
    HKEY value = nullptr;
};

void CheckRegistry(LSTATUS status, const wchar_t* operation) {
    if (status != ERROR_SUCCESS) {
        throw winrt::hresult_error(HRESULT_FROM_WIN32(status), operation);
    }
}

std::optional<DWORD> ReadDword(const wchar_t* name) {
    DWORD value = 0, bytes = sizeof(value);
    const auto status = RegGetValueW(
        HKEY_CURRENT_USER, SettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return std::nullopt;
    CheckRegistry(status, L"Cannot read PaperShade preferences.");
    return value;
}

std::wstring StartupCommand() {
    return L"\"" + ExecutablePath() + L"\" --startup";
}

}

Settings LoadSettings() {
    Settings result;
    if (const auto value = ReadDword(L"Preset")) {
        if (!ValidPreset(*value)) throw winrt::hresult_invalid_argument(L"Stored preset is invalid. Use --reset-settings.");
        result.preset = static_cast<Preset>(*value);
    }
    if (const auto value = ReadDword(L"Fps")) {
        if (!ValidFps(*value)) throw winrt::hresult_invalid_argument(L"Stored frame cap is invalid. Use --reset-settings.");
        result.fps = *value;
    }
    if (const auto value = ReadDword(L"PixelSize")) {
        if (*value < 1 || *value > 4) throw winrt::hresult_invalid_argument(L"Stored pattern size is invalid. Use --reset-settings.");
        result.pixelSize = *value;
    }
    if (const auto value = ReadDword(L"Enabled")) {
        if (*value > 1) throw winrt::hresult_invalid_argument(L"Stored enabled state is invalid. Use --reset-settings.");
        result.enabled = *value != 0;
    }
    if (const auto value = ReadDword(L"HideCaptureIndicator")) {
        if (*value > 1) throw winrt::hresult_invalid_argument(L"Stored capture-indicator preference is invalid. Use --reset-settings.");
        result.hideCaptureIndicator = *value != 0;
    }
    return result;
}

void SaveSettings(const Settings& settings) {
    Key key;
    CheckRegistry(RegCreateKeyExW(HKEY_CURRENT_USER, SettingsKey, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key.value, nullptr), L"Cannot open PaperShade preferences.");
    const std::pair<const wchar_t*, DWORD> values[]{
        {L"Preset", static_cast<DWORD>(settings.preset)}, {L"Fps", settings.fps},
        {L"PixelSize", settings.pixelSize}, {L"Enabled", settings.enabled ? 1u : 0u},
        {L"HideCaptureIndicator", settings.hideCaptureIndicator ? 1u : 0u}
    };
    for (const auto& [name, value] : values) {
        CheckRegistry(RegSetValueExW(key.value, name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value)), L"Cannot save PaperShade preferences.");
    }
}

bool StartsWithWindows() {
    DWORD bytes = 0;
    auto status = RegGetValueW(HKEY_CURRENT_USER, StartupKey, L"PaperShade",
        RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
    if (status == ERROR_FILE_NOT_FOUND) return false;
    CheckRegistry(status, L"Cannot read Windows startup preference.");
    std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    CheckRegistry(RegGetValueW(HKEY_CURRENT_USER, StartupKey, L"PaperShade",
        RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes), L"Cannot read Windows startup command.");
    return std::wstring(buffer.data()) == StartupCommand();
}

void SetStartsWithWindows(bool enabled) {
    Key key;
    CheckRegistry(RegCreateKeyExW(HKEY_CURRENT_USER, StartupKey, 0, nullptr, 0,
        KEY_SET_VALUE, nullptr, &key.value, nullptr), L"Cannot open Windows startup preferences.");
    if (enabled) {
        const auto command = StartupCommand();
        CheckRegistry(RegSetValueExW(key.value, L"PaperShade", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))),
            L"Cannot enable startup with Windows.");
    } else {
        const auto status = RegDeleteValueW(key.value, L"PaperShade");
        if (status != ERROR_FILE_NOT_FOUND) CheckRegistry(status, L"Cannot disable startup with Windows.");
    }
}

}
