/**
 * @file ShaderTranspileCacheTest.cpp
 * @brief Tests the on-disk shader transpile cache without a GL context.
 *
 * The cache is a process-wide singleton that reads its configuration once, so this
 * file points it at a fresh temporary directory with a 1 MiB budget before main()
 * runs, and fills that directory with "old" entries as another process would have,
 * so the startup scan sees them.
 */
#include <MilkdropPreset/ShaderTranspileCache.hpp>

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using libprojectM::MilkdropPreset::ShaderTranspileCache;

namespace {

constexpr int OldEntries = 100;
constexpr std::size_t EntryBytes = 8 * 1024;

auto TestDirectory() -> const std::filesystem::path&
{
#if defined(_WIN32)
    static const auto directory = std::filesystem::temp_directory_path() / ("projectm-cache-test-" + std::to_string(_getpid()));
#else
    static const auto directory = std::filesystem::temp_directory_path() / ("projectm-cache-test-" + std::to_string(getpid()));
#endif
    return directory;
}

void SetEnvironment(const char* name, const std::string& value)
{
#if defined(_WIN32)
    _putenv_s(name, value.c_str());
#else
    if (value.empty())
    {
        unsetenv(name);
    }
    else
    {
        setenv(name, value.c_str(), 1);
    }
#endif
}

/// GLSL-looking text of exactly EntryBytes, distinct per tag.
auto Glsl(const std::string& tag) -> std::string
{
    std::string text = "// " + tag + "\nvoid main() {}\n";
    text.resize(EntryBytes, ' ');
    return text;
}

auto OldName(int index) -> std::string
{
    char name[32];
    std::snprintf(name, sizeof(name), "old-%03d", index);
    return name;
}

auto EntryPath(const std::string& key) -> std::filesystem::path
{
    return TestDirectory() / (key + ".glsl");
}

void WriteFile(const std::filesystem::path& path, const std::string& contents)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

/// Runs before main(): the configuration and an aged directory, 800 KiB of a
/// 1 MiB budget, old-000 the least recently used.
struct Setup
{
    Setup()
    {
        std::error_code error;
        std::filesystem::remove_all(TestDirectory(), error);
        std::filesystem::create_directories(TestDirectory(), error);
        SetEnvironment("PROJECTM_SHADER_CACHE", "");
        SetEnvironment("PROJECTM_SHADER_CACHE_VERIFY", "");
        SetEnvironment("PROJECTM_SHADER_CACHE_DIR", TestDirectory().string());
        SetEnvironment("PROJECTM_SHADER_CACHE_MAX_MB", "1");

        const auto now = std::filesystem::file_time_type::clock::now();
        for (int index = 0; index < OldEntries; ++index)
        {
            const auto path = EntryPath(OldName(index));
            WriteFile(path, Glsl(OldName(index)));
            std::filesystem::last_write_time(path, now - std::chrono::minutes(300 - index), error);
        }

        // A temporary file left by a process that died mid-write.
        const auto stale = TestDirectory() / "dead.glsl.tmp.1";
        WriteFile(stale, "partial");
        std::filesystem::last_write_time(stale, now - std::chrono::hours(2), error);
    }

    ~Setup()
    {
        std::error_code error;
        std::filesystem::remove_all(TestDirectory(), error);
    }
} setup;

auto& Cache()
{
    return ShaderTranspileCache::Instance();
}

} // namespace

TEST(ShaderTranspileCache, StartupScanRemovesStaleTemporaryFiles)
{
    ASSERT_TRUE(Cache().Enabled());
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_FALSE(std::filesystem::exists(TestDirectory() / "dead.glsl.tmp.1"));
    // Under the budget at start: nothing else is touched.
    EXPECT_TRUE(std::filesystem::exists(EntryPath(OldName(0))));
}

TEST(ShaderTranspileCache, PrunesLeastRecentlyUsedFirst)
{
    ASSERT_TRUE(Cache().Flush(5000));

    // A hit on the oldest entry makes it the most recently used one.
    std::string glsl;
    ASSERT_TRUE(Cache().Lookup(OldName(0), glsl));
    EXPECT_EQ(glsl, Glsl(OldName(0)));
    ASSERT_TRUE(Cache().Flush(5000));
    const auto touched = std::filesystem::last_write_time(EntryPath(OldName(0)));
    EXPECT_GT(touched, std::filesystem::file_time_type::clock::now() - std::chrono::minutes(1));

    // 40 new entries take the directory to 1120 KiB, over the 1 MiB budget.
    for (int index = 0; index < 40; ++index)
    {
        Cache().Store("new-" + std::to_string(index), Glsl("new-" + std::to_string(index)));
    }
    ASSERT_TRUE(Cache().Flush(10000));

    EXPECT_TRUE(std::filesystem::exists(EntryPath(OldName(0)))) << "the entry that was just used was pruned";
    EXPECT_FALSE(std::filesystem::exists(EntryPath(OldName(1)))) << "the least recently used entry was kept";
    EXPECT_TRUE(std::filesystem::exists(EntryPath(OldName(OldEntries - 1))));
    EXPECT_TRUE(std::filesystem::exists(EntryPath("new-39")));

    std::uintmax_t total = 0;
    for (const auto& item : std::filesystem::directory_iterator(TestDirectory()))
    {
        total += item.file_size();
    }
    EXPECT_LE(total, 1024u * 1024u);
}

TEST(ShaderTranspileCache, AStoreHitsBeforeAndAfterItIsWritten)
{
    const std::string key = "store-hit";
    Cache().Store(key, Glsl(key));

    std::string glsl;
    EXPECT_TRUE(Cache().Lookup(key, glsl)) << "a queued store is not served from memory";
    EXPECT_EQ(glsl, Glsl(key));

    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_TRUE(std::filesystem::exists(EntryPath(key)));
    EXPECT_TRUE(Cache().Lookup(key, glsl));
    EXPECT_EQ(glsl, Glsl(key));

    Cache().Discard(key);
    EXPECT_FALSE(Cache().Lookup(key, glsl)) << "a queued discard still hit";
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_FALSE(std::filesystem::exists(EntryPath(key)));
}

TEST(ShaderTranspileCache, ADamagedEntryIsAMiss)
{
    std::string glsl;
    WriteFile(EntryPath("empty"), "");
    EXPECT_FALSE(Cache().Lookup("empty", glsl));

    WriteFile(EntryPath("garbage"), "not a shader at all");
    EXPECT_FALSE(Cache().Lookup("garbage", glsl));

    EXPECT_FALSE(Cache().Lookup("missing", glsl));
    EXPECT_TRUE(glsl.empty());
}

TEST(ShaderTranspileCache, ARejectEntryBelongsToOneDriver)
{
    const std::string key = "rejected-shader";
    const std::string driver = "Vendor | Renderer | 3.3.0 Driver 1.0 | 3.30";
    std::string reason;

    EXPECT_FALSE(Cache().LookupRejected(key, driver, reason));
    Cache().StoreRejected(key, driver, "driver: error C1068: array index out of bounds\nmore lines");

    EXPECT_TRUE(Cache().LookupRejected(key, driver, reason)) << "a queued reject is not served from memory";
    EXPECT_EQ(reason, "driver: error C1068: array index out of bounds");
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_TRUE(Cache().LookupRejected(key, driver, reason));

    // An updated driver tries the shader again.
    EXPECT_FALSE(Cache().LookupRejected(key, "Vendor | Renderer | 3.3.0 Driver 2.0 | 3.30", reason));
    // A reject is not a cached GLSL entry.
    std::string glsl;
    EXPECT_FALSE(Cache().Lookup(key, glsl));

    Cache().DiscardRejected(key, driver);
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_FALSE(Cache().LookupRejected(key, driver, reason));
}

TEST(ShaderTranspileCache, TheKeyCoversEveryInput)
{
    const auto base = Cache().Key("warp", 330, 1, "uniform sampler2D sampler_main;", "void PS() {}");
    EXPECT_EQ(base, Cache().Key("warp", 330, 1, "uniform sampler2D sampler_main;", "void PS() {}"));
    EXPECT_NE(base, Cache().Key("composite", 330, 1, "uniform sampler2D sampler_main;", "void PS() {}"));
    EXPECT_NE(base, Cache().Key("warp", 300, 1, "uniform sampler2D sampler_main;", "void PS() {}"));
    EXPECT_NE(base, Cache().Key("warp", 330, 2, "uniform sampler2D sampler_main;", "void PS() {}"));
    EXPECT_NE(base, Cache().Key("warp", 330, 1, "", "void PS() {}"));
    EXPECT_NE(base, Cache().Key("warp", 330, 1, "uniform sampler2D sampler_main;", "void PS() { }"));
    EXPECT_EQ(base.size(), 64u);
}
