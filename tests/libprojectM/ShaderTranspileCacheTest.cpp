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

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using libprojectM::MilkdropPreset::ShaderTranspileCache;

namespace {

/// A reject entry another process wrote at HitWritten, 2 hours before the test starts.
constexpr const char* HitKey = "often-hit-shader";
constexpr const char* HitDriver = "Vendor | Hits | 3.3.0 Driver 1.0 | 3.30";
constexpr std::int64_t HitWritten = 1800000000;

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

        // A reject entry as another process wrote it, so this process has never
        // touched it and the worker's recency throttle does not apply.
        const auto rejectPath = TestDirectory() / ShaderTranspileCache::RejectEntryName(HitKey, HitDriver);
        WriteFile(rejectPath, std::string("projectm-shader-reject 2\n") + HitDriver + "\nwritten " + std::to_string(HitWritten) +
                                  "\ntranspile: error\n");
        std::filesystem::last_write_time(rejectPath, now - std::chrono::hours(2), error);

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

/// The fake wall clock the reject tests date and age entries with.
std::atomic<std::int64_t> fakeNow{1700000000};

auto FakeClock() -> std::int64_t
{
    return fakeNow.load();
}

constexpr std::int64_t Day = 24 * 60 * 60;

/// Installs the fake clock for one test and restores the system clock after it.
struct FakeClockScope
{
    explicit FakeClockScope(std::int64_t start)
    {
        fakeNow = start;
        Cache().SetWallClockForTesting(&FakeClock);
    }

    ~FakeClockScope()
    {
        Cache().SetWallClockForTesting(nullptr);
    }
};

/// The reject entry file whose driver line is `driver`, or an empty path.
auto RejectFileFor(const std::string& driver) -> std::filesystem::path
{
    for (const auto& item : std::filesystem::directory_iterator(TestDirectory()))
    {
        if (item.path().extension() != ".reject")
        {
            continue;
        }
        std::ifstream file(item.path(), std::ios::binary);
        const std::string contents((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        if (contents.find("\n" + driver + "\n") != std::string::npos)
        {
            return item.path();
        }
    }
    return {};
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

TEST(ShaderTranspileCache, ARejectEntryExpiresAWeekAfterItWasWritten)
{
    const FakeClockScope clock(1700000000);
    const std::string key = "expiring-shader";
    const std::string driver = "Vendor | Expiry | 3.3.0 Driver 1.0 | 3.30";
    std::string reason;

    Cache().StoreRejected(key, driver, "driver: error");
    ASSERT_TRUE(Cache().Flush(5000));

    fakeNow += ShaderTranspileCache::RejectLifetimeSeconds - 1;
    EXPECT_TRUE(Cache().LookupRejected(key, driver, reason)) << "expired a second early";
    EXPECT_EQ(reason, "driver: error");

    fakeNow += 1;
    EXPECT_FALSE(Cache().LookupRejected(key, driver, reason)) << "a week-old reject entry was still trusted";
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_TRUE(RejectFileFor(driver).empty()) << "the expired entry was not removed";

    // The retry failed again: a new entry, dated now, lives another week.
    Cache().StoreRejected(key, driver, "driver: error again");
    ASSERT_TRUE(Cache().Flush(5000));
    fakeNow += 6 * Day;
    EXPECT_TRUE(Cache().LookupRejected(key, driver, reason));
    EXPECT_EQ(reason, "driver: error again");
    fakeNow += Day;
    EXPECT_FALSE(Cache().LookupRejected(key, driver, reason));
    ASSERT_TRUE(Cache().Flush(5000));
}

TEST(ShaderTranspileCache, AHitDoesNotExtendARejectEntry)
{
    const FakeClockScope clock(HitWritten);
    const std::string driver = HitDriver;
    std::string reason;
    ASSERT_TRUE(Cache().Flush(5000));
    const auto path = TestDirectory() / ShaderTranspileCache::RejectEntryName(HitKey, driver);
    const auto before = std::filesystem::last_write_time(path);

    // Hit it every day for six days, as often as the app loads the preset.
    for (int day = 0; day < 7; ++day)
    {
        fakeNow = HitWritten + day * Day;
        EXPECT_TRUE(Cache().LookupRejected(HitKey, driver, reason)) << "day " << day;
        EXPECT_EQ(reason, "transpile: error");
    }
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_EQ(std::filesystem::last_write_time(path), before) << "a hit refreshed the entry's recency";

    // Exactly 7 days after the write: the hits did not move that.
    fakeNow = HitWritten + 7 * Day - 1;
    EXPECT_TRUE(Cache().LookupRejected(HitKey, driver, reason));
    fakeNow = HitWritten + 7 * Day;
    EXPECT_FALSE(Cache().LookupRejected(HitKey, driver, reason)) << "the hits extended the entry's life";
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ShaderTranspileCache, ARejectEntryFromTheFutureIsNotTrusted)
{
    const FakeClockScope clock(1900000000);
    const std::string key = "future-shader";
    const std::string driver = "Vendor | Future | 3.3.0 Driver 1.0 | 3.30";
    std::string reason;

    Cache().StoreRejected(key, driver, "driver: error");
    ASSERT_TRUE(Cache().Flush(5000));

    // A clock a little behind another process's is fine.
    fakeNow -= ShaderTranspileCache::RejectFutureToleranceSeconds;
    EXPECT_TRUE(Cache().LookupRejected(key, driver, reason));

    // A clock set back by a year must not keep the entry for a year and a week.
    fakeNow -= 365 * Day;
    EXPECT_FALSE(Cache().LookupRejected(key, driver, reason));
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_TRUE(RejectFileFor(driver).empty());
}

TEST(ShaderTranspileCache, AnUndatedRejectEntryIsTriedAgain)
{
    const FakeClockScope clock(2000000000);
    const std::string key = "undated-shader";
    const std::string driver = "Vendor | Undated | 3.3.0 Driver 1.0 | 3.30";
    std::string reason;

    Cache().StoreRejected(key, driver, "driver: error");
    ASSERT_TRUE(Cache().Flush(5000));
    const auto path = RejectFileFor(driver);
    ASSERT_FALSE(path.empty());

    // What a build before reject entries were dated wrote under the same name.
    WriteFile(path, "projectm-shader-reject 1\n" + driver + "\ndriver: error\n");
    EXPECT_FALSE(Cache().LookupRejected(key, driver, reason));
    ASSERT_TRUE(Cache().Flush(5000));
    EXPECT_FALSE(std::filesystem::exists(path));

    // A dated entry with a garbled date is no better.
    Cache().StoreRejected(key, driver, "driver: error");
    ASSERT_TRUE(Cache().Flush(5000));
    WriteFile(path, "projectm-shader-reject 2\n" + driver + "\nwritten soon\ndriver: error\n");
    EXPECT_FALSE(Cache().LookupRejected(key, driver, reason));
    ASSERT_TRUE(Cache().Flush(5000));
}
