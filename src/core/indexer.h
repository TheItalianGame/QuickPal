#pragma once

#include "everything_client.h"
#include "types.h"

#include <shared_mutex>
#include <string>
#include <vector>

// The window that receives kIndexUpdatedMessage when background work changes state.
void setIndexNotifyWindow(HWND hwnd);

void rebuildIndexAsync();
void buildFileIndexAsync();
void rebuildIndexBlocking(bool includeFallbackFileIndex = false);

// Status has three layers: a stable index base, a provider-owned operation line,
// and a transient UI line. The most specific non-empty layer is painted.
void setStatus(const std::wstring& value);
void setProviderStatus(const std::wstring& providerId, const std::wstring& value);
void clearProviderStatus(const std::wstring& providerId = {});
void setTransientStatus(const std::wstring& value);
void clearTransientStatus();
std::wstring getStatus();
std::wstring getBaseStatus();

bool everythingReady();
bool everythingHttpReady();
// The search path clears a backend when a live query fails, so one that goes away
// mid-session falls back instead of retrying every keystroke.
void setEverythingReady(bool ready);
void setEverythingHttpReady(bool ready);
bool fileIndexing();
int staticCommandCount();
int indexedFileCount();
EverythingSdkClient& everythingClient();
EverythingHttpClient& everythingHttpClient();

// Callers must hold a shared lock on indexMutex() while touching either vector.
std::shared_mutex& indexMutex();
const std::vector<Command>& staticIndexUnlocked();
const std::vector<Command>& fileIndexUnlocked();
