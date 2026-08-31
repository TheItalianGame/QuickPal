#include "providers.h"

#include "../settings.h"
#include "../util.h"

#include <bcrypt.h>
#include <objbase.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr wchar_t kGenerateGuidArg[] = L"value:generate-guid";
constexpr wchar_t kBase64Alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::wstring generateGuid()
{
    GUID guid{};
    if (FAILED(CoCreateGuid(&guid)))
    {
        return {};
    }

    wchar_t buffer[64]{};
    if (StringFromGUID2(guid, buffer, 64) <= 0)
    {
        return {};
    }

    std::wstring text(buffer);
    if (text.size() >= 2 && text.front() == L'{' && text.back() == L'}')
    {
        text = text.substr(1, text.size() - 2);
    }
    return lowerCopy(text);
}

std::wstring base64Encode(const std::string& bytes)
{
    std::wstring out;
    out.reserve(((bytes.size() + 2) / 3) * 4);

    for (size_t i = 0; i < bytes.size(); i += 3)
    {
        const uint32_t a = static_cast<unsigned char>(bytes[i]);
        const uint32_t b = (i + 1 < bytes.size()) ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const uint32_t c = (i + 2 < bytes.size()) ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const uint32_t n = (a << 16) | (b << 8) | c;

        out.push_back(kBase64Alphabet[(n >> 18) & 0x3F]);
        out.push_back(kBase64Alphabet[(n >> 12) & 0x3F]);
        out.push_back(i + 1 < bytes.size() ? kBase64Alphabet[(n >> 6) & 0x3F] : L'=');
        out.push_back(i + 2 < bytes.size() ? kBase64Alphabet[n & 0x3F] : L'=');
    }
    return out;
}

int base64Index(wchar_t ch)
{
    if (ch >= L'A' && ch <= L'Z') return ch - L'A';
    if (ch >= L'a' && ch <= L'z') return 26 + ch - L'a';
    if (ch >= L'0' && ch <= L'9') return 52 + ch - L'0';
    if (ch == L'+') return 62;
    if (ch == L'/') return 63;
    return -1;
}

std::optional<std::string> base64Decode(const std::wstring& text)
{
    std::wstring clean;
    clean.reserve(text.size());
    for (wchar_t ch : text)
    {
        if (!std::iswspace(ch))
        {
            clean.push_back(ch);
        }
    }
    if (clean.empty() || clean.size() % 4 != 0)
    {
        return std::nullopt;
    }

    std::string out;
    out.reserve((clean.size() / 4) * 3);
    for (size_t i = 0; i < clean.size(); i += 4)
    {
        const int a = base64Index(clean[i]);
        const int b = base64Index(clean[i + 1]);
        const int c = clean[i + 2] == L'=' ? -2 : base64Index(clean[i + 2]);
        const int d = clean[i + 3] == L'=' ? -2 : base64Index(clean[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1)
        {
            return std::nullopt;
        }

        const uint32_t n = (static_cast<uint32_t>(a) << 18) |
                           (static_cast<uint32_t>(b) << 12) |
                           (static_cast<uint32_t>(std::max(c, 0)) << 6) |
                           static_cast<uint32_t>(std::max(d, 0));
        out.push_back(static_cast<char>((n >> 16) & 0xFF));
        if (c != -2)
        {
            out.push_back(static_cast<char>((n >> 8) & 0xFF));
        }
        if (d != -2)
        {
            out.push_back(static_cast<char>(n & 0xFF));
        }
    }
    return out;
}

std::wstring hexBytes(const std::vector<unsigned char>& bytes)
{
    static constexpr wchar_t kHex[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes)
    {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

std::optional<std::wstring> hashText(const wchar_t* algorithm, const std::wstring& text)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, 0) < 0)
    {
        return std::nullopt;
    }

    DWORD objectBytes = 0;
    DWORD hashBytes = 0;
    DWORD returned = 0;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectBytes),
                          sizeof(objectBytes), &returned, 0) < 0 ||
        BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashBytes),
                          sizeof(hashBytes), &returned, 0) < 0 ||
        objectBytes == 0 || hashBytes == 0)
    {
        BCryptCloseAlgorithmProvider(alg, 0);
        return std::nullopt;
    }

    std::vector<unsigned char> object(objectBytes);
    std::vector<unsigned char> hash(hashBytes);
    BCRYPT_HASH_HANDLE handle = nullptr;
    const std::string bytes = toUtf8(text);
    bool ok = BCryptCreateHash(alg, &handle, object.data(), objectBytes, nullptr, 0, 0) >= 0;
    ok = ok && BCryptHashData(handle, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                              static_cast<ULONG>(bytes.size()), 0) >= 0;
    ok = ok && BCryptFinishHash(handle, hash.data(), hashBytes, 0) >= 0;
    if (handle)
    {
        BCryptDestroyHash(handle);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok ? std::optional<std::wstring>(hexBytes(hash)) : std::nullopt;
}

std::wstring commandBody(const Query& q, std::wstring& verb)
{
    if (q.hasPrefix())
    {
        verb = lowerCopy(q.prefix);
        if (verb == L"value")
        {
            const size_t firstSpace = q.body.find_first_of(L" \t");
            if (firstSpace == std::wstring::npos)
            {
                verb = lowerCopy(q.body);
                return {};
            }
            verb = lowerCopy(trimCopy(q.body.substr(0, firstSpace)));
            return trimCopy(q.body.substr(firstSpace + 1));
        }
        return q.body;
    }
    if (q.terms.empty())
    {
        verb.clear();
        return {};
    }

    verb = q.terms.front();
    const size_t firstSpace = q.raw.find_first_of(L" \t");
    return firstSpace == std::wstring::npos ? std::wstring{} : trimCopy(q.raw.substr(firstSpace + 1));
}

void addValue(ResultSink& sink, const std::wstring& title, const std::wstring& subtitle,
              const std::wstring& value, int score, const std::wstring& stableKey)
{
    if (value.empty())
    {
        return;
    }
    Command command = makeCommand(CommandKind::ValueTool, title, subtitle, value, 7600);
    if (!stableKey.empty())
    {
        command.key = L"value|" + stableKey;
    }
    sink.add(std::move(command), score);
}

void addPrompts(ResultSink& sink)
{
    Command guid = makeCommand(CommandKind::ValueTool, L"Generate GUID",
                               L"Create a UUID and copy it", kGenerateGuidArg, 7600);
    guid.searchText += L" uuid value";
    guid.key = L"value|guid";
    sink.add(std::move(guid), 19000);
    sink.add(makeCommand(CommandKind::PaletteQuery, L"Base64 encode text",
                         L"Type text after base64", L"base64 ", 3600), 18000);
    sink.add(makeCommand(CommandKind::PaletteQuery, L"Decode Base64 text",
                         L"Type text after from64", L"from64 ", 3550), 17900);
    sink.add(makeCommand(CommandKind::PaletteQuery, L"SHA256 hash text",
                         L"Type text after sha256", L"sha256 ", 3500), 17800);
}

bool isValueTextVerb(const std::wstring& verb)
{
    return verb == L"base64" || verb == L"b64" || verb == L"unbase64" || verb == L"from64" ||
           verb == L"sha256" || verb == L"sha1" || verb == L"md5";
}

class ValuesProvider : public Provider
{
public:
    ProviderInfo info() const override
    {
        ProviderInfo info;
        info.id = L"values";
        info.title = L"Value tools";
        info.prefixes = { L"value", L"guid", L"uuid", L"base64", L"b64", L"unbase64", L"from64", L"sha256", L"sha1", L"md5" };
        info.mode = QueryMode::Values;
        info.exclusive = true;
        info.runsUnprefixed = true;
        return info;
    }

    void index(const ProviderContext&, std::vector<Command>& out) override
    {
        (void)out;
    }

    void query(const ProviderContext&, const Query& q, ResultSink& sink) override
    {
        if (!q.hasPrefix() && q.raw.empty())
        {
            return;
        }

        std::wstring verb;
        const std::wstring body = commandBody(q, verb);
        if (verb.empty())
        {
            if (q.hasPrefix())
            {
                addPrompts(sink);
            }
            return;
        }

        if (verb == L"guid" || verb == L"uuid")
        {
            addValue(sink, L"Generate GUID", L"Enter copies a fresh UUID", generateGuid(), 26000, L"guid");
            return;
        }

        if (!isValueTextVerb(verb))
        {
            return;
        }

        if (body.empty())
        {
            sink.add(makeCommand(CommandKind::ValueTool, L"Type text for " + verb,
                                 L"Example: " + verb + L" hello", L"", 0), 7000);
            return;
        }

        if (verb == L"base64" || verb == L"b64")
        {
            addValue(sink, L"Base64 encode", body, base64Encode(toUtf8(body)), 25000, L"base64");
            return;
        }
        if (verb == L"unbase64" || verb == L"from64")
        {
            if (auto decoded = base64Decode(body))
            {
                addValue(sink, L"Base64 decode", body, fromUtf8(*decoded), 25000, L"from64");
            }
            return;
        }
        if (verb == L"sha256")
        {
            if (auto hash = hashText(BCRYPT_SHA256_ALGORITHM, body))
            {
                addValue(sink, L"SHA256 hash", body, *hash, 25000, L"sha256");
            }
            return;
        }
        if (verb == L"sha1")
        {
            if (auto hash = hashText(BCRYPT_SHA1_ALGORITHM, body))
            {
                addValue(sink, L"SHA1 hash", body, *hash, 25000, L"sha1");
            }
            return;
        }
        if (verb == L"md5")
        {
            if (auto hash = hashText(BCRYPT_MD5_ALGORITHM, body))
            {
                addValue(sink, L"MD5 hash", body, *hash, 25000, L"md5");
            }
        }
    }

    bool execute(const ProviderContext& ctx, const Command& command) override
    {
        if (command.kind != CommandKind::ValueTool)
        {
            return false;
        }
        const std::wstring value = command.arg == kGenerateGuidArg ? generateGuid() : command.arg;
        if (!value.empty())
        {
            copyTextToClipboard(ctx.window, value);
        }
        return true;
    }

    void settings(const ProviderContext&, std::vector<SettingRow>& out) override
    {
        out.push_back(makeSettingHeader(L"Value tools"));
        out.push_back(makeSettingItem(SettingField::ProviderShortcut, SettingKind::Action,
                                      L"Shortcut", L"Open generators and converters directly", info().id));
        out.push_back(makeSettingItem(SettingField::ProviderPrefix, SettingKind::Action,
                                      L"Prefix", L"Typed alias for value tools", info().id));
    }
};
}

std::unique_ptr<Provider> makeValuesProvider()
{
    return std::make_unique<ValuesProvider>();
}
