#include "providers.h"

void registerBuiltinProviders()
{
    ProviderRegistry& registry = ProviderRegistry::instance();
    if (!registry.all().empty())
    {
        return;
    }

    // Index-time providers: contribute a static candidate set that the shared
    // fuzzy scorer ranks.
    registry.add(makeBuiltinsProvider());
    registry.add(makeSettingsUriProvider());
    registry.add(makeStartMenuProvider());
    registry.add(makeAppsFolderProvider());
    registry.add(makePathToolsProvider());
    registry.add(makeQuickLinksProvider());

    // Query-time providers: compute results from the query itself.
    registry.add(makeFilesProvider());
    registry.add(makeWindowsProvider());
    registry.add(makeShellProvider());
    registry.add(makeWebProvider());
    registry.add(makeCalculatorProvider());
    registry.add(makeProcessesProvider());
    registry.add(makeClipboardProvider());
}
