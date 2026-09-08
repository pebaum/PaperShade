#pragma once

#include <Windows.h>
#include <cstdint>
#include <optional>

namespace paper {
std::optional<std::uint32_t> ShowWarmthDialog(HWND owner, std::uint32_t currentKelvin);
}
