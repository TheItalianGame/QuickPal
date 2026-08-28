#include "app.h"

#include "core/settings.h"

AppState g_app;

float pxToDip(int pixels, UINT dpi)
{
    const UINT effective = dpi == 0 ? 96 : dpi;
    return static_cast<float>(pixels) * 96.0f / static_cast<float>(effective);
}

int dipToPx(float dip, UINT dpi)
{
    const UINT effective = dpi == 0 ? 96 : dpi;
    return static_cast<int>(dip * static_cast<float>(effective) / 96.0f + 0.5f);
}

void refreshTheme()
{
    g_app.theme = resolveTheme(getSettingsSnapshot());
    g_app.graphics.setHighlightColor(g_app.theme.accent);
}
