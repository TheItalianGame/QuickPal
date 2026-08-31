#pragma once

#include "layout.h"

#include <d2d1.h>

// Client size in DIPs, which is the coordinate space the whole UI is authored in.
D2D1_SIZE_F clientSizeDip();

// Both the painter and the hit-tester call these, so a spacing change can never
// leave clicks pointing at where things used to be drawn.
PaletteLayout buildPaletteLayout();
SettingsLayout buildSettingsLayout();
const SettingSection& activeSettingSection();
const std::vector<SettingRow>& activeSettingRows();

// Draws one frame. Handles device loss internally; returns false if the device
// could not be (re)created.
bool paintFrame();
