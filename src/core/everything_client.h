#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <string>
#include <vector>

struct FileResultEntry
{
    std::wstring path;
    std::wstring sizeText;
    std::wstring modifiedText;
    bool hasType = false;
    bool isFolder = false;
};

struct EverythingHttpSettings
{
    std::wstring host = L"127.0.0.1";
    std::wstring username;
    std::wstring password;
    int port = 2342;
};

struct EverythingHttpSearchResult
{
    bool ok = false;
    DWORD statusCode = 0;
    std::vector<FileResultEntry> entries;
};

struct EverythingSdkSearchResult
{
    bool ok = false;
    std::vector<FileResultEntry> entries;
};

std::wstring formatFileSize(ULONGLONG bytes);
std::wstring formatFileTime(const FILETIME& fileTime);
FileResultEntry fileEntryFromPath(const std::wstring& path);
std::wstring fileEntrySubtitle(const FileResultEntry& entry);

class EverythingSdkClient
{
public:
    bool load();
    bool loaded() const;
    std::wstring loadedPath() const;
    EverythingSdkSearchResult search(const std::wstring& query, DWORD maxResults);

private:
    using SetSearchFn = void(WINAPI*)(LPCWSTR);
    using SetRequestFlagsFn = void(WINAPI*)(DWORD);
    using SetMaxFn = void(WINAPI*)(DWORD);
    using QueryFn = BOOL(WINAPI*)(BOOL);
    using GetNumResultsFn = DWORD(WINAPI*)();
    using GetFullPathFn = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
    using GetSizeFn = BOOL(WINAPI*)(DWORD, LARGE_INTEGER*);
    using GetDateModifiedFn = BOOL(WINAPI*)(DWORD, FILETIME*);
    using IsFolderResultFn = BOOL(WINAPI*)(DWORD);

    HMODULE module_ = nullptr;
    bool attempted_ = false;
    bool loaded_ = false;
    std::wstring loadedPath_;
    SetSearchFn setSearch_ = nullptr;
    SetRequestFlagsFn setRequestFlags_ = nullptr;
    SetMaxFn setMax_ = nullptr;
    QueryFn query_ = nullptr;
    GetNumResultsFn getNumResults_ = nullptr;
    GetFullPathFn getFullPath_ = nullptr;
    GetSizeFn getSize_ = nullptr;
    GetDateModifiedFn getDateModified_ = nullptr;
    IsFolderResultFn isFolderResult_ = nullptr;
};

class EverythingHttpClient
{
public:
    EverythingHttpClient();
    ~EverythingHttpClient();

    EverythingHttpSearchResult search(const EverythingHttpSettings& settings, const std::wstring& query, int maxResults);

private:
    HINTERNET session_ = nullptr;
    std::atomic_ullong skipUntilMs_{ 0 };
};
