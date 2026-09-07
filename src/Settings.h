#pragma once

#include "Filters.h"

namespace paper {

Settings LoadSettings();
void SaveSettings(const Settings& settings);
bool StartsWithWindows();
void SetStartsWithWindows(bool enabled);

}
