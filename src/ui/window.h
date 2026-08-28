#pragma once

#include "../core/types.h"

bool registerWindowClass(HINSTANCE instance);
HWND createMainWindow(HINSTANCE instance);

void showPalette();
void showSettings();
void hidePalette();
void refreshResults();
void positionWindow();

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
