#include "WarmthDialog.h"
#include "Filters.h"
#include "Resource.h"
#include "WinUtil.h"
#include <commctrl.h>
#include <iterator>

namespace paper {
namespace {

std::optional<std::uint32_t> ReadKelvin(HWND dialog) {
    wchar_t text[16]{};
    const int length = GetDlgItemTextW(dialog, IDC_KELVIN_VALUE, text, static_cast<int>(std::size(text)));
    if (length != 4) return std::nullopt;
    std::uint32_t value = 0;
    for (int index = 0; index < length; ++index) {
        if (text[index] < L'0' || text[index] > L'9') return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(text[index] - L'0');
    }
    return ValidKelvin(value) ? std::optional<std::uint32_t>{value} : std::nullopt;
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* kelvin = reinterpret_cast<std::uint32_t*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        kelvin = reinterpret_cast<std::uint32_t*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(kelvin));
        SendDlgItemMessageW(dialog, IDC_KELVIN_SLIDER, TBM_SETRANGE, TRUE,
            MAKELPARAM(MinimumKelvin, NeutralKelvin));
        SendDlgItemMessageW(dialog, IDC_KELVIN_SLIDER, TBM_SETTICFREQ, 500, 0);
        SendDlgItemMessageW(dialog, IDC_KELVIN_SLIDER, TBM_SETPAGESIZE, 0, 500);
        SendDlgItemMessageW(dialog, IDC_KELVIN_SLIDER, TBM_SETPOS, TRUE, *kelvin);
        SetDlgItemInt(dialog, IDC_KELVIN_VALUE, *kelvin, FALSE);
        SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return TRUE;
    }
    if (!kelvin) return FALSE;
    if (message == WM_HSCROLL && reinterpret_cast<HWND>(lparam) == GetDlgItem(dialog, IDC_KELVIN_SLIDER)) {
        const auto value = static_cast<UINT>(SendDlgItemMessageW(dialog, IDC_KELVIN_SLIDER, TBM_GETPOS, 0, 0));
        SetDlgItemInt(dialog, IDC_KELVIN_VALUE, value, FALSE);
        SetDlgItemTextW(dialog, IDC_KELVIN_ERROR, L"");
        return TRUE;
    }
    if (message == WM_COMMAND) {
        switch (LOWORD(wparam)) {
        case IDC_KELVIN_VALUE:
            if (HIWORD(wparam) == EN_CHANGE) {
                if (const auto value = ReadKelvin(dialog)) {
                    SendDlgItemMessageW(dialog, IDC_KELVIN_SLIDER, TBM_SETPOS, TRUE, *value);
                    SetDlgItemTextW(dialog, IDC_KELVIN_ERROR, L"");
                }
            }
            return TRUE;
        case IDC_KELVIN_NEUTRAL:
            SetDlgItemInt(dialog, IDC_KELVIN_VALUE, NeutralKelvin, FALSE);
            return TRUE;
        case IDOK: {
            const auto value = ReadKelvin(dialog);
            if (!value) {
                SetDlgItemTextW(dialog, IDC_KELVIN_ERROR, L"Enter a whole number from 1000 to 6500 K.");
                return TRUE;
            }
            *kelvin = *value;
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
    }
    if (message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}

}

std::optional<std::uint32_t> ShowWarmthDialog(HWND owner, std::uint32_t currentKelvin) {
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    CheckWin32(InitCommonControlsEx(&controls), L"Cannot initialize the temperature slider.");
    const auto result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_WARMTH),
        owner, DialogProc, reinterpret_cast<LPARAM>(&currentKelvin));
    CheckWin32(result != -1, L"Cannot open the color-temperature dialog.");
    if (result == IDOK) return currentKelvin;
    return std::nullopt;
}

}
