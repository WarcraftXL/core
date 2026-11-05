// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-storage-test.exe: names, the redirect/provider/transform order and the in-memory file handle.

#include "engine/storage/ArchiveName.hpp"
#include "engine/storage/Providers.hpp"
#include "engine/storage/SyntheticFile.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    int g_failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++g_failures; } } while (0)

    namespace st = wxl::storage;

    std::string g_calls;

    void Write(const WXL_ByteSink* sink, const char* text) { sink->Write(sink->ctx, text, uint32_t(std::strlen(text))); }

    int __cdecl RedirectNone(const char*, char*, uint32_t) { g_calls += "0"; return 0; }
    int __cdecl RedirectEmpty(const char*, char* out, uint32_t) { g_calls += "e"; out[0] = '\0'; return 1; }
    int __cdecl RedirectModel(const char* name, char* out, uint32_t capacity)
    {
        g_calls += "m";
        if (!st::EndsWithCI(name, ".m2")) return 0;
        std::snprintf(out, capacity, "Other\\%s", name);
        return 1;
    }
    int __cdecl RedirectLong(const char*, char* out, uint32_t capacity)
    {
        std::memset(out, 'x', capacity);
        return 1;
    }

    int __cdecl ProvideWritesThenDeclines(const char*, const WXL_ByteSink* sink)
    {
        g_calls += "d";
        Write(sink, "junk");
        return 0;
    }
    int __cdecl ProvideFirst(const char* name, const WXL_ByteSink* sink)
    {
        g_calls += "1";
        if (std::strcmp(name, "a.txt") != 0) return 0;
        Write(sink, "one");
        return 1;
    }
    int __cdecl ProvideSecond(const char*, const WXL_ByteSink* sink)
    {
        g_calls += "2";
        Write(sink, "two");
        return 1;
    }

    int __cdecl TransformBlp(const char*, const uint8_t* raw, uint32_t size, const WXL_ByteSink* sink)
    {
        g_calls += "b";
        sink->Write(sink->ctx, raw, size);
        Write(sink, "+blp");
        return 1;
    }
    int __cdecl TransformDeclines(const char*, const uint8_t*, uint32_t, const WXL_ByteSink*)
    {
        g_calls += "n";
        return 0;
    }

    std::string Text(const std::vector<uint8_t>& bytes) { return std::string(bytes.begin(), bytes.end()); }

    void Names()
    {
        CHECK(st::EndsWithCI("World\\Maps\\A.BLP", ".blp"));
        CHECK(st::EndsWithCI("a/b.m2", "\\B.M2"));
        CHECK(!st::EndsWithCI("m2", ".m2"));

        st::NameBuffer copy;
        CHECK(st::CopyArchiveName("Interface\\Glues\\x.blp", copy) && std::strcmp(copy, "Interface\\Glues\\x.blp") == 0);
        CHECK(!st::CopyArchiveName(nullptr, copy));
        CHECK(!st::CopyArchiveName("", copy));
        CHECK(!st::CopyArchiveName("\x01", copy));
        CHECK(!st::CopyArchiveName("caf\xE9", copy));
        const std::string tooLong(st::kMaxArchiveName, 'a');
        CHECK(!st::CopyArchiveName(tooLong.c_str(), copy));

        CHECK(st::ShouldIntercept("a.m2") && st::ShouldIntercept("a.blp"));
        CHECK(!st::ShouldIntercept("a.WAV") && !st::ShouldIntercept("x.pub") && !st::ShouldIntercept("map.tex"));
    }

    void Redirects()
    {
        st::Reset();
        char out[st::kMaxRedirectName];
        CHECK(!st::Redirect("a.m2", out));

        CHECK(st::AddRedirect(&RedirectNone) && st::AddRedirect(&RedirectEmpty) && st::AddRedirect(&RedirectModel));
        CHECK(!st::AddRedirect(nullptr));
        g_calls.clear();
        CHECK(st::Redirect("a.m2", out) && std::strcmp(out, "Other\\a.m2") == 0 && g_calls == "0em");
        g_calls.clear();
        CHECK(!st::Redirect("a.blp", out) && g_calls == "0em");

        st::Reset();
        CHECK(st::AddRedirect(&RedirectLong));
        CHECK(st::Redirect("a", out) && std::strlen(out) == st::kMaxRedirectName - 1);
    }

    void Providers()
    {
        st::Reset();
        std::vector<uint8_t> bytes;
        CHECK(!st::Provide("a.txt", bytes));

        CHECK(st::AddProvider(&ProvideWritesThenDeclines) && st::AddProvider(&ProvideFirst) &&
              st::AddProvider(&ProvideSecond));
        g_calls.clear();
        CHECK(st::Provide("a.txt", bytes) && Text(bytes) == "one" && g_calls == "d1");
        g_calls.clear();
        CHECK(st::Provide("b.txt", bytes) && Text(bytes) == "two" && g_calls == "d12");

        st::Reset();
        CHECK(st::AddProvider(&ProvideWritesThenDeclines));
        CHECK(!st::Provide("a.txt", bytes) && bytes.empty());

        for (uint32_t i = 1; i < st::kMaxRegistrations; ++i) CHECK(st::AddProvider(&ProvideFirst));
        CHECK(!st::AddProvider(&ProvideFirst));
    }

    void Transforms()
    {
        st::Reset();
        CHECK(!st::HasTransform("a.blp"));
        CHECK(!st::AddTransform("", &TransformBlp) && !st::AddTransform(nullptr, &TransformBlp));
        CHECK(st::AddTransform(".m2", &TransformDeclines) && st::AddTransform(".BLP", &TransformDeclines) &&
              st::AddTransform(".blp", &TransformBlp));
        CHECK(st::HasTransform("x\\a.blp") && st::HasTransform("a.M2") && !st::HasTransform("a.wmo"));

        const uint8_t        raw[] = { 'r', 'a', 'w' };
        std::vector<uint8_t> bytes;
        g_calls.clear();
        CHECK(st::Transform("a.blp", raw, sizeof raw, bytes) && Text(bytes) == "raw+blp" && g_calls == "nb");
        g_calls.clear();
        CHECK(!st::Transform("a.m2", raw, sizeof raw, bytes) && bytes.empty() && g_calls == "n");
    }

    void Files()
    {
        const uint8_t       data[] = { 1, 2, 3, 4, 5 };
        st::SyntheticFile* file   = st::MakeSyntheticFile("a.blp", data, sizeof data);
        CHECK(file && st::IsSynthetic(file) && file->size == 5 && std::strcmp(file->fullName, "a.blp") == 0);
        const uint32_t native[4] = { 1, 0, 0, 0 };
        CHECK(!st::IsSynthetic(native) && !st::IsSynthetic(nullptr));

        uint8_t  buffer[8] = {};
        uint32_t read      = 0;
        CHECK(st::ReadSynthetic(*file, buffer, 2, &read) && read == 2 && buffer[1] == 2);
        CHECK(!st::ReadSynthetic(*file, buffer, 8, &read) && read == 3 && buffer[0] == 3 && file->position == 5);
        CHECK(st::SeekSynthetic(*file, -2, st::SeekFrom::End) == 3);
        CHECK(st::SeekSynthetic(*file, 1, st::SeekFrom::Current) == 4);
        CHECK(st::SeekSynthetic(*file, -9, st::SeekFrom::Current) == 0);
        CHECK(st::SeekSynthetic(*file, 99, st::SeekFrom::Begin) == 5);
        CHECK(st::SeekSynthetic(*file, 2, st::SeekFrom(7)) == 2);
        st::FreeSynthetic(file);

        st::SyntheticFile* empty = st::MakeSyntheticFile("e", nullptr, 0);
        CHECK(empty && empty->size == 0 && st::ReadSynthetic(*empty, buffer, 0, &read) && read == 0);
        st::FreeSynthetic(empty);
    }
}

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Names();
    Redirects();
    Providers();
    Transforms();
    Files();
    std::printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
