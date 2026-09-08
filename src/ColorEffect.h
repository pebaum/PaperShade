#pragma once

#include "Filters.h"
#include <memory>
#include <string>

namespace paper {

class ColorEffect {
public:
    ColorEffect();
    ~ColorEffect();
    ColorEffect(const ColorEffect&) = delete;
    ColorEffect& operator=(const ColorEffect&) = delete;
    void Apply(Preset preset, std::uint32_t kelvin = NeutralKelvin);
    void Restore();
    bool Active() const noexcept;
    static int Guardian(unsigned long parentId, const std::wstring& mappingName);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
