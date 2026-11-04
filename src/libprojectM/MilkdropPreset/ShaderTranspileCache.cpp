/**
 * @file ShaderTranspileCache.cpp
 * @brief Implementation of the on-disk cache for transpiled preset shaders.
 */
#include "ShaderTranspileCache.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace libprojectM {
namespace MilkdropPreset {

namespace {

/// Largest entry we ever read back. Generated preset GLSL is 20 to 200 KB.
constexpr std::uintmax_t MaxEntryBytes = 4 * 1024 * 1024;

/// Entries kept before the oldest are pruned.
constexpr std::size_t MaxEntries = 4096;

/// Bytes kept before the oldest entries are pruned.
constexpr std::uintmax_t MaxBytes = 64 * 1024 * 1024;

/// How many stores pass between two prune passes.
constexpr std::uint64_t StoresBetweenPrunes = 64;

/**
 * @brief Minimal SHA-256, used to name cache entries.
 *
 * Self-contained so the cache adds no dependency. A key only has to be stable and
 * collision-free for distinct inputs, and SHA-256 gives that with a few microseconds
 * of work on a preset-sized shader.
 */
class Sha256
{
public:
    void Update(const void* data, std::size_t length)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        m_bitLength += static_cast<std::uint64_t>(length) * 8;

        while (length > 0)
        {
            const std::size_t chunk = std::min(length, sizeof(m_buffer) - m_bufferLength);
            std::memcpy(m_buffer.data() + m_bufferLength, bytes, chunk);
            m_bufferLength += chunk;
            bytes += chunk;
            length -= chunk;

            if (m_bufferLength == sizeof(m_buffer))
            {
                Transform(m_buffer.data());
                m_bufferLength = 0;
            }
        }
    }

    auto FinalHex() -> std::string
    {
        const std::uint64_t bitLength = m_bitLength;

        // Padding: 0x80, then zeros, then the 64-bit big-endian length.
        const std::uint8_t one = 0x80;
        Update(&one, 1);
        const std::uint8_t zero = 0;
        while (m_bufferLength != 56)
        {
            Update(&zero, 1);
        }

        std::uint8_t lengthBytes[8];
        for (int i = 0; i < 8; ++i)
        {
            lengthBytes[7 - i] = static_cast<std::uint8_t>((bitLength >> (8 * i)) & 0xFF);
        }
        Update(lengthBytes, sizeof(lengthBytes));

        static constexpr char digits[] = "0123456789abcdef";
        std::string hex;
        hex.reserve(64);
        for (const auto word : m_state)
        {
            for (int shift = 28; shift >= 0; shift -= 4)
            {
                hex.push_back(digits[(word >> shift) & 0xF]);
            }
        }
        return hex;
    }

private:
    static auto RotateRight(std::uint32_t value, int bits) -> std::uint32_t
    {
        return (value >> bits) | (value << (32 - bits));
    }

    void Transform(const std::uint8_t* block)
    {
        static constexpr std::uint32_t constants[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        std::uint32_t schedule[64];
        for (int i = 0; i < 16; ++i)
        {
            schedule[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
                          (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
                          (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
                          static_cast<std::uint32_t>(block[i * 4 + 3]);
        }
        for (int i = 16; i < 64; ++i)
        {
            const std::uint32_t s0 = RotateRight(schedule[i - 15], 7) ^ RotateRight(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
            const std::uint32_t s1 = RotateRight(schedule[i - 2], 17) ^ RotateRight(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
        }

        std::uint32_t a = m_state[0];
        std::uint32_t b = m_state[1];
        std::uint32_t c = m_state[2];
        std::uint32_t d = m_state[3];
        std::uint32_t e = m_state[4];
        std::uint32_t f = m_state[5];
        std::uint32_t g = m_state[6];
        std::uint32_t h = m_state[7];

        for (int i = 0; i < 64; ++i)
        {
            const std::uint32_t s1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + choice + constants[i] + schedule[i];
            const std::uint32_t s0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + majority;

            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }

        m_state[0] += a;
        m_state[1] += b;
        m_state[2] += c;
        m_state[3] += d;
        m_state[4] += e;
        m_state[5] += f;
        m_state[6] += g;
        m_state[7] += h;
    }

    std::array<std::uint32_t, 8> m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<std::uint8_t, 64> m_buffer{};
    std::size_t m_bufferLength{};
    std::uint64_t m_bitLength{};
};

void UpdateInt32(Sha256& hash, int value)
{
    const auto unsignedValue = static_cast<std::uint32_t>(value);
    std::uint8_t bytes[4] = {
        static_cast<std::uint8_t>((unsignedValue >> 24) & 0xFF),
        static_cast<std::uint8_t>((unsignedValue >> 16) & 0xFF),
        static_cast<std::uint8_t>((unsignedValue >> 8) & 0xFF),
        static_cast<std::uint8_t>(unsignedValue & 0xFF)};
    hash.Update(bytes, sizeof(bytes));
}

void UpdateString(Sha256& hash, const std::string& value)
{
    // Length-prefixed, so that no two distinct part sequences hash the same bytes.
    const auto length = static_cast<std::uint64_t>(value.size());
    std::uint8_t bytes[8];
    for (int i = 0; i < 8; ++i)
    {
        bytes[7 - i] = static_cast<std::uint8_t>((length >> (8 * i)) & 0xFF);
    }
    hash.Update(bytes, sizeof(bytes));
    hash.Update(value.data(), value.size());
}

auto EnvValue(const char* name) -> std::string
{
    std::string text;
#if defined(_WIN32)
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, name) != 0 || buffer == nullptr)
    {
        return {};
    }
    text.assign(buffer);
    std::free(buffer);
#else
    const char* value = std::getenv(name);
    if (value == nullptr)
    {
        return {};
    }
    text.assign(value);
#endif

    // Trim surrounding whitespace: `set NAME=1 && ...` in a batch file leaves a
    // trailing space in the value, and an empty value is the same as unset.
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

auto EnvTruthy(const char* name) -> bool
{
    const std::string value = EnvValue(name);
    return !(value.empty() || value == "0" || value == "false" || value == "no" || value == "off");
}

auto ProcessId() -> long
{
#if defined(_WIN32)
    return static_cast<long>(_getpid());
#else
    return static_cast<long>(getpid());
#endif
}

auto NowMilliseconds() -> double
{
    using Clock = std::chrono::steady_clock;
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

} // namespace

struct ShaderTranspileCache::Impl
{
    bool enabled{false};
    bool verify{false};
    bool debug{false};
    std::string directory;
    std::string disabledReason;

    std::mutex mutex;
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t unreadable{};
    std::uint64_t storeFailures{};
    std::uint64_t compileFailures{};
    std::uint64_t verifyMismatches{};
    std::uint64_t entriesPruned{};
    std::uint64_t stores{};
    double hitMilliseconds{};
    double storeMilliseconds{};
    double transpileMilliseconds{};
    bool compileFailureLogged{false};

    Impl()
    {
        // Read the diagnostic switches first: they also apply to a disabled cache,
        // where the per-shader phase numbers are the "before" measurement.
        verify = EnvTruthy("PROJECTM_SHADER_CACHE_VERIFY");
        debug = EnvTruthy("PROJECTM_SHADER_CACHE_DEBUG");

        // The cache is on by default; only an explicit false value turns it off.
        const std::string disableValue = EnvValue("PROJECTM_SHADER_CACHE");
        if (!disableValue.empty() && !EnvTruthy("PROJECTM_SHADER_CACHE"))
        {
            disabledReason = "PROJECTM_SHADER_CACHE is set to a false value";
            LogDisabled();
            return;
        }

        std::filesystem::path cacheDirectory;
        const std::string overrideDirectory = EnvValue("PROJECTM_SHADER_CACHE_DIR");
        if (!overrideDirectory.empty())
        {
            cacheDirectory = std::filesystem::path(overrideDirectory);
        }
        else
        {
#if defined(_WIN32)
            const std::string localAppData = EnvValue("LOCALAPPDATA");
            if (!localAppData.empty())
            {
                cacheDirectory = std::filesystem::path(localAppData) / "projectm" / "shader-cache";
            }
#else
            const std::string xdgCache = EnvValue("XDG_CACHE_HOME");
            const std::string home = EnvValue("HOME");
            if (!xdgCache.empty())
            {
                cacheDirectory = std::filesystem::path(xdgCache) / "projectm" / "shader-cache";
            }
            else if (!home.empty())
            {
                cacheDirectory = std::filesystem::path(home) / ".cache" / "projectm" / "shader-cache";
            }
#endif
            if (cacheDirectory.empty())
            {
                disabledReason = "no platform cache directory in the environment";
                LogDisabled();
                return;
            }
        }

        std::error_code error;
        std::filesystem::create_directories(cacheDirectory, error);
        if (error)
        {
            disabledReason = "cannot create " + cacheDirectory.string() + ": " + error.message();
            LogDisabled();
            return;
        }

        directory = cacheDirectory.string();
        enabled = true;

        if (debug)
        {
            ShaderCacheDebugLog("[projectm shader cache] enabled, directory " + directory +
                                (verify ? " (verify mode: every hit is transpiled again)" : ""));
        }
    }

    ~Impl()
    {
        PrintSummary();
    }

    void LogDisabled() const
    {
        if (debug)
        {
            ShaderCacheDebugLog("[projectm shader cache] disabled: " + disabledReason);
        }
    }

    auto EnabledEntryPath(const std::string& key) const -> std::filesystem::path
    {
        return std::filesystem::path(directory) / (key + ".glsl");
    }

    void PrintSummary()
    {
        if (!enabled || !debug)
        {
            return;
        }

        const std::uint64_t lookups = hits + misses;
        const double hitRate = lookups == 0 ? 0.0 : (100.0 * static_cast<double>(hits) / static_cast<double>(lookups));
        const double averageMiss = misses == 0 ? 0.0 : transpileMilliseconds / static_cast<double>(misses);
        const double estimatedSaved = static_cast<double>(hits) * averageMiss - hitMilliseconds;

        char line[512];
        std::snprintf(line, sizeof(line),
                      "[projectm shader cache] summary: directory=%s lookups=%llu hits=%llu misses=%llu hit-rate=%.1f%% "
                      "unreadable=%llu store-failures=%llu compile-failures=%llu verify-mismatches=%llu pruned=%llu",
                      directory.c_str(),
                      static_cast<unsigned long long>(lookups),
                      static_cast<unsigned long long>(hits),
                      static_cast<unsigned long long>(misses),
                      hitRate,
                      static_cast<unsigned long long>(unreadable),
                      static_cast<unsigned long long>(storeFailures),
                      static_cast<unsigned long long>(compileFailures),
                      static_cast<unsigned long long>(verifyMismatches),
                      static_cast<unsigned long long>(entriesPruned));
        ShaderCacheDebugLog(line);

        std::snprintf(line, sizeof(line),
                      "[projectm shader cache] timings: hit-read=%.1fms store=%.1fms transpiled=%.1fms over %llu misses "
                      "(%.1fms each) estimated-saved=%.1fms",
                      hitMilliseconds,
                      storeMilliseconds,
                      transpileMilliseconds,
                      static_cast<unsigned long long>(misses),
                      averageMiss,
                      estimatedSaved);
        ShaderCacheDebugLog(line);
    }

    void PruneIfNeeded()
    {
        if (stores == 0 || stores % StoresBetweenPrunes != 0)
        {
            return;
        }

        struct Entry
        {
            std::filesystem::path path;
            std::filesystem::file_time_type modified;
            std::uintmax_t size{};
        };

        std::vector<Entry> entries;
        std::uintmax_t totalBytes = 0;

        std::error_code error;
        for (const auto& item : std::filesystem::directory_iterator(directory, error))
        {
            if (error)
            {
                break;
            }
            const auto& path = item.path();
            const std::string name = path.filename().string();
            if (name.empty())
            {
                continue;
            }
            const bool isEntry = name.size() > 5 && name.compare(name.size() - 5, 5, ".glsl") == 0;
            const bool isTemporary = name.find(".tmp") != std::string::npos;

            std::error_code entryError;
            if (isTemporary)
            {
                // Leftover from a process that died between write and rename.
                const auto modified = std::filesystem::last_write_time(path, entryError);
                if (!entryError && std::filesystem::file_time_type::clock::now() - modified > std::chrono::hours(1))
                {
                    std::filesystem::remove(path, entryError);
                }
                continue;
            }
            if (!isEntry)
            {
                continue;
            }

            Entry entry;
            entry.path = path;
            entry.modified = std::filesystem::last_write_time(path, entryError);
            if (entryError)
            {
                continue;
            }
            entry.size = std::filesystem::file_size(path, entryError);
            if (entryError)
            {
                continue;
            }
            totalBytes += entry.size;
            entries.push_back(std::move(entry));
        }

        if (entries.size() <= MaxEntries && totalBytes <= MaxBytes)
        {
            return;
        }

        std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
            return left.modified < right.modified;
        });

        std::size_t index = 0;
        while (index < entries.size() &&
               (entries.size() - index > MaxEntries || totalBytes > MaxBytes))
        {
            std::error_code removeError;
            if (std::filesystem::remove(entries[index].path, removeError))
            {
                totalBytes -= std::min(totalBytes, entries[index].size);
                ++entriesPruned;
            }
            ++index;
        }

        ShaderCacheDebugLog("[projectm shader cache] pruned " + std::to_string(index) + " entries, " +
                            std::to_string(totalBytes) + " bytes kept");
    }
};

auto ShaderTranspileCache::Instance() -> ShaderTranspileCache&
{
    static ShaderTranspileCache instance;
    return instance;
}

ShaderTranspileCache::ShaderTranspileCache()
    : m_impl(new Impl())
{
}

ShaderTranspileCache::~ShaderTranspileCache()
{
    delete m_impl;
}

auto ShaderTranspileCache::Enabled() const -> bool
{
    return m_impl->enabled;
}

auto ShaderTranspileCache::VerifyEnabled() const -> bool
{
    return m_impl->verify;
}

auto ShaderTranspileCache::DebugEnabled() const -> bool
{
    return m_impl->debug;
}

auto ShaderTranspileCache::Directory() const -> const std::string&
{
    return m_impl->directory;
}

auto ShaderTranspileCache::DisabledReason() const -> const std::string&
{
    return m_impl->disabledReason;
}

auto ShaderTranspileCache::Key(const std::string& shaderType,
                               int glslVersion,
                               int maxBlurLevel,
                               const std::string& declarations,
                               const std::string& program) const -> std::string
{
    Sha256 hash;
    UpdateString(hash, "projectm-preset-shader-transpile-cache");
    UpdateInt32(hash, CacheFormatVersion);
    UpdateString(hash, shaderType);
    UpdateInt32(hash, glslVersion);
    UpdateInt32(hash, maxBlurLevel);
    UpdateString(hash, declarations);
    UpdateString(hash, program);
    return hash.FinalHex();
}

auto ShaderTranspileCache::Lookup(const std::string& key, std::string& glsl) -> bool
{
    glsl.clear();
    if (!m_impl->enabled)
    {
        return false;
    }

    const double start = NowMilliseconds();
    const auto path = m_impl->EnabledEntryPath(key);

    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error || size == 0 || size > MaxEntryBytes)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (!error)
        {
            // An empty or absurdly large file can only be garbage.
            ++m_impl->unreadable;
        }
        ++m_impl->misses;
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    std::string contents;
    contents.resize(static_cast<std::size_t>(size));
    if (!file.read(&contents[0], static_cast<std::streamsize>(size)))
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        ++m_impl->unreadable;
        ++m_impl->misses;
        return false;
    }
    file.close();

    // Cheap sanity check: every generated preset shader has a main() function.
    // Anything else is a damaged entry, and a miss costs only one transpile.
    if (contents.find("void main()") == std::string::npos)
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        ++m_impl->unreadable;
        ++m_impl->misses;
        return false;
    }

    glsl = std::move(contents);

    const double elapsed = NowMilliseconds() - start;
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    ++m_impl->hits;
    m_impl->hitMilliseconds += elapsed;
    return true;
}

void ShaderTranspileCache::Store(const std::string& key, const std::string& glsl)
{
    if (!m_impl->enabled || glsl.empty())
    {
        return;
    }

    const double start = NowMilliseconds();

    // The whole write is under the lock: the temporary name is per process, so two
    // threads storing at once would otherwise share it.
    std::lock_guard<std::mutex> lock(m_impl->mutex);

    const auto path = m_impl->EnabledEntryPath(key);
    const auto temporary = std::filesystem::path(path.string() + ".tmp." + std::to_string(ProcessId()));

    bool wrote = false;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (file)
        {
            file.write(glsl.data(), static_cast<std::streamsize>(glsl.size()));
            file.flush();
            wrote = file.good();
            file.close();
        }
    }

    std::error_code error;
    if (wrote)
    {
        std::filesystem::rename(temporary, path, error);
        if (error)
        {
            // Windows refuses to rename onto an existing file on some file systems.
            std::error_code removeError;
            std::filesystem::remove(path, removeError);
            error.clear();
            std::filesystem::rename(temporary, path, error);
        }
    }

    if (!wrote || error)
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        ++m_impl->storeFailures;
        if (m_impl->storeFailures == 1 && m_impl->debug)
        {
            ShaderCacheDebugLog("[projectm shader cache] cannot write " + path.string() +
                                (error ? ": " + error.message() : std::string()));
        }
        return;
    }

    m_impl->storeMilliseconds += NowMilliseconds() - start;
    ++m_impl->stores;
    m_impl->PruneIfNeeded();
}

void ShaderTranspileCache::Discard(const std::string& key)
{
    if (!m_impl->enabled)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    std::error_code error;
    std::filesystem::remove(m_impl->EnabledEntryPath(key), error);
}

void ShaderTranspileCache::RecordVerification(bool matched, const std::string& key)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (matched)
    {
        return;
    }

    ++m_impl->verifyMismatches;
    ShaderCacheDebugLog("[projectm shader cache] VERIFY MISMATCH: the cached GLSL for key " + key +
                        " differs from a fresh transpile; the cache key is missing an input");
}

void ShaderTranspileCache::RecordCompileFailure(const std::string& key)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    ++m_impl->compileFailures;
    if (!m_impl->compileFailureLogged)
    {
        m_impl->compileFailureLogged = true;
        ShaderCacheDebugLog("[projectm shader cache] cached GLSL for key " + key +
                            " did not compile; transpiled again and replaced the entry");
    }
}

void ShaderTranspileCache::RecordTranspileTime(double milliseconds)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->transpileMilliseconds += milliseconds;
}

void ShaderCacheDebugLog(const std::string& line)
{
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

} // namespace MilkdropPreset
} // namespace libprojectM
