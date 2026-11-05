/**
 * @file ShaderTranspileCache.cpp
 * @brief Implementation of the on-disk cache for transpiled preset shaders.
 */
#include "ShaderTranspileCache.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace libprojectM {
namespace MilkdropPreset {

namespace {

/// Largest entry we ever read back. Generated preset GLSL is 5 to 12 KB.
constexpr std::uintmax_t MaxEntryBytes = 4 * 1024 * 1024;

/// Largest reject entry we ever read back: a header, the driver strings and a reason.
constexpr std::uintmax_t MaxRejectBytes = 64 * 1024;

/// A prune removes entries until the directory is at this fraction of the budget,
/// so a full cache is not pruned again on the very next store.
constexpr double PruneTargetFraction = 0.9;

/// Stores between two directory rescans. The worker keeps its own running total;
/// a rescan picks up entries other processes added and removed in the meantime.
constexpr std::uint64_t StoresBetweenRescans = 1024;

/// Queued work the caller threads may hand the worker before new work is dropped.
/// A dropped store is only a later miss, so this bounds memory, nothing else.
constexpr std::size_t MaxQueuedBytes = 32 * 1024 * 1024;
constexpr std::size_t MaxQueuedJobs = 65536;

/// A hit refreshes an entry's modification time at most this often.
constexpr auto TouchInterval = std::chrono::minutes(10);

/// The worker stops after this long without work, and starts again on demand.
constexpr auto WorkerIdleExit = std::chrono::seconds(5);

/// At process exit, queued writes get this long to reach the disk; the rest is dropped.
constexpr auto ShutdownDrain = std::chrono::seconds(2);

/// Temporary files older than this are leftovers of a process that died mid-write.
constexpr auto StaleTemporary = std::chrono::hours(1);

/// First line of a reject entry. Version 1 had no date; such an entry is a miss,
/// so the shader is tried once more and a failure writes a dated one.
constexpr const char* RejectMagic = "projectm-shader-reject 2";

/// Third line of a reject entry, followed by the Unix time it was written.
constexpr const char* RejectWrittenPrefix = "written ";

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

/// Seconds since the Unix epoch, from the system clock.
auto SystemWallClock() -> std::int64_t
{
    return static_cast<std::int64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

/// Reads a whole file of at most `limit` bytes. False on any error or an empty file.
auto ReadSmallFile(const std::filesystem::path& path, std::uintmax_t limit, std::string& contents, bool& damaged) -> bool
{
    damaged = false;
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (error)
    {
        return false;
    }
    if (size == 0 || size > limit)
    {
        // An empty or absurdly large file can only be garbage.
        damaged = true;
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    contents.resize(static_cast<std::size_t>(size));
    if (!file.read(&contents[0], static_cast<std::streamsize>(size)))
    {
        damaged = true;
        contents.clear();
        return false;
    }
    return true;
}

/// The first line of a possibly multi-line message.
auto FirstLine(const std::string& text) -> std::string
{
    const auto end = text.find_first_of("\r\n");
    return end == std::string::npos ? text : text.substr(0, end);
}

} // namespace

/**
 * @brief The cache's state.
 *
 * Caller threads (the render thread) only read entries and queue work under
 * `mutex`. One worker thread at a time does every write, rename, mtime update,
 * directory scan and prune. The members marked "worker only" are touched by the
 * worker alone; a new worker is only started after the previous one has been
 * joined, which orders their accesses.
 */
struct ShaderTranspileCache::Impl
{
    struct Job
    {
        enum class Kind
        {
            Store,  //!< Write `payload` to `name`, atomically.
            Touch,  //!< Refresh the modification time of `name`.
            Discard //!< Remove `name`.
        };

        Kind kind{Kind::Store};
        std::string name;
        std::shared_ptr<const std::string> payload;
    };

    struct IndexEntry
    {
        std::uintmax_t size{};
        std::filesystem::file_time_type modified{};
    };

    bool enabled{false};
    bool verify{false};
    bool debug{false};
    std::string directory;
    std::string disabledReason;
    std::uint64_t maxBytes{DefaultMaxBytes};

    // Shared between the caller threads and the worker, guarded by `mutex`.
    std::mutex mutex;
    std::condition_variable wake; //!< The worker waits on it for work or the stop.
    std::condition_variable idle; //!< Flush() and shutdown wait on it for the worker.
    std::deque<Job> queue;
    std::size_t queuedBytes{};
    //! Stores queued but not yet on disk, so a lookup in the meantime still hits.
    std::unordered_map<std::string, std::shared_ptr<const std::string>> pending;
    //! When this process last asked for an entry's mtime to be refreshed.
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> touchRequested;
    std::thread worker;
    bool workerRunning{false};
    bool workerBusy{false};
    bool stopping{false};
    bool startupScanDone{false};
    std::chrono::steady_clock::time_point stopDeadline{};
    //! Dates and ages reject entries; null for the system clock. Tests replace it.
    ShaderTranspileCache::WallClock wallClock{nullptr};

    // Counters, guarded by `mutex`.
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t unreadable{};
    std::uint64_t storeFailures{};
    std::uint64_t compileFailures{};
    std::uint64_t verifyMismatches{};
    std::uint64_t rejectHits{};
    std::uint64_t rejectsStored{};
    std::uint64_t rejectVerifyMismatches{};
    std::uint64_t rejectsExpired{};
    std::uint64_t entriesPruned{};
    std::uint64_t stores{};
    std::uint64_t touches{};
    std::uint64_t droppedJobs{};
    std::uint64_t scans{};
    std::uint64_t lastScanEntries{};
    std::uint64_t lastScanBytes{};
    std::uint64_t startupEntries{};
    std::uint64_t startupBytes{};
    double hitMilliseconds{};
    double maxHitMilliseconds{};
    double storeMilliseconds{};
    double transpileMilliseconds{};
    double scanMilliseconds{};
    double startupScanMilliseconds{};
    bool compileFailureLogged{false};

    // Worker only.
    std::unordered_map<std::string, IndexEntry> index;
    std::uintmax_t indexBytes{};
    std::uint64_t storesSinceScan{};
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastTouched;

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

        const std::string budget = EnvValue("PROJECTM_SHADER_CACHE_MAX_MB");
        if (!budget.empty())
        {
            char* end = nullptr;
            const unsigned long long megabytes = std::strtoull(budget.c_str(), &end, 10);
            if (end != nullptr && *end == '\0' && megabytes > 0)
            {
                maxBytes = static_cast<std::uint64_t>(megabytes) * 1024 * 1024;
            }
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
                                ", budget " + std::to_string(maxBytes / (1024 * 1024)) + " MiB" +
                                (verify ? " (verify mode: every hit is transpiled again)" : ""));
        }

        // Prune at start: the worker's first job is the directory scan, which also
        // deletes stale temporary files and trims the directory to the budget.
        std::lock_guard<std::mutex> lock(mutex);
        StartWorkerLocked();
    }

    /**
     * @brief Stops the worker, giving queued writes ShutdownDrain to finish.
     * @return true if the worker is gone and the state may be freed.
     */
    auto Shutdown() -> bool
    {
        std::unique_lock<std::mutex> lock(mutex);
        stopping = true;
        stopDeadline = std::chrono::steady_clock::now() + ShutdownDrain;
        wake.notify_all();
        if (workerRunning)
        {
            idle.wait_until(lock, stopDeadline + std::chrono::milliseconds(500), [this] { return !workerRunning; });
        }
        const bool finished = !workerRunning;
        lock.unlock();

        if (worker.joinable())
        {
            if (finished)
            {
                worker.join();
            }
            else
            {
                // Stuck on a slow disk: let it go with the process rather than hang
                // the exit. The state stays allocated for it.
                worker.detach();
            }
        }

        PrintSummary();
        return finished;
    }

    void LogDisabled() const
    {
        if (debug)
        {
            ShaderCacheDebugLog("[projectm shader cache] disabled: " + disabledReason);
        }
    }

    auto EntryPath(const std::string& name) const -> std::filesystem::path
    {
        return std::filesystem::path(directory) / name;
    }

    /// Starts the worker unless one is running. Called with `mutex` held.
    void StartWorkerLocked()
    {
        if (workerRunning || stopping)
        {
            wake.notify_one();
            return;
        }
        if (worker.joinable())
        {
            // The previous worker has set workerRunning to false and is returning;
            // it needs no lock to do so.
            worker.join();
        }
        try
        {
            worker = std::thread([this] { Run(); });
            workerRunning = true;
        }
        catch (...)
        {
            // No thread: the queued work stays queued and is retried with the next
            // job. Nothing on the caller's thread waits for it.
        }
    }

    /// Queues a job for the worker. Called with `mutex` held.
    void EnqueueLocked(Job job)
    {
        if (stopping)
        {
            return;
        }
        const std::size_t bytes = job.payload ? job.payload->size() : 0;
        if (queue.size() >= MaxQueuedJobs || queuedBytes + bytes > MaxQueuedBytes)
        {
            ++droppedJobs;
            return;
        }
        if (job.kind == Job::Kind::Store)
        {
            pending[job.name] = job.payload;
        }
        else if (job.kind == Job::Kind::Discard)
        {
            pending.erase(job.name);
            touchRequested.erase(job.name);
        }
        queuedBytes += bytes;
        queue.push_back(std::move(job));
        StartWorkerLocked();
    }

    /// Asks the worker to refresh an entry's recency, at most every TouchInterval.
    /// Called with `mutex` held; costs a map lookup, no I/O.
    void RequestTouchLocked(const std::string& name)
    {
        const auto now = std::chrono::steady_clock::now();
        auto found = touchRequested.find(name);
        if (found != touchRequested.end() && now - found->second < TouchInterval)
        {
            return;
        }
        touchRequested[name] = now;
        Job job;
        job.kind = Job::Kind::Touch;
        job.name = name;
        EnqueueLocked(std::move(job));
    }

    /// The wall clock reject entries are dated with. Called with `mutex` held.
    auto WallSecondsLocked() const -> std::int64_t
    {
        return wallClock != nullptr ? wallClock() : SystemWallClock();
    }

    /// Returns a queued store's payload, if one is waiting for the worker.
    /// Called with `mutex` held.
    auto PendingLocked(const std::string& name) const -> std::shared_ptr<const std::string>
    {
        auto found = pending.find(name);
        return found == pending.end() ? nullptr : found->second;
    }

    /// The worker thread: the startup scan, then every queued job in order.
    void Run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        if (!startupScanDone && !stopping)
        {
            lock.unlock();
            const double start = NowMilliseconds();
            try
            {
                ScanAndPrune();
            }
            catch (...)
            {
            }
            const double elapsed = NowMilliseconds() - start;
            lock.lock();
            startupScanDone = true;
            startupScanMilliseconds = elapsed;
            startupEntries = lastScanEntries;
            startupBytes = lastScanBytes;
        }

        while (true)
        {
            if (queue.empty())
            {
                idle.notify_all();
                if (stopping)
                {
                    break;
                }
                wake.wait_for(lock, WorkerIdleExit, [this] { return !queue.empty() || stopping; });
                if (queue.empty())
                {
                    break;
                }
            }
            if (stopping && std::chrono::steady_clock::now() > stopDeadline)
            {
                // Out of time at exit: what is left is only a later miss.
                droppedJobs += queue.size();
                queue.clear();
                pending.clear();
                queuedBytes = 0;
                break;
            }

            Job job = std::move(queue.front());
            queue.pop_front();
            queuedBytes -= job.payload ? job.payload->size() : 0;
            workerBusy = true;
            lock.unlock();

            try
            {
                Process(job);
            }
            catch (...)
            {
                // The cache is only a cache: nothing it does may end the process.
            }

            lock.lock();
            workerBusy = false;
            if (job.kind == Job::Kind::Store)
            {
                auto found = pending.find(job.name);
                if (found != pending.end() && found->second == job.payload)
                {
                    pending.erase(found);
                }
            }
        }

        workerRunning = false;
        idle.notify_all();
    }

    /// Does one job. Worker only, without `mutex`, except to update counters.
    void Process(const Job& job)
    {
        switch (job.kind)
        {
            case Job::Kind::Store:
                Write(job.name, *job.payload);
                break;

            case Job::Kind::Touch:
                Touch(job.name);
                break;

            case Job::Kind::Discard:
            {
                std::error_code error;
                std::filesystem::remove(EntryPath(job.name), error);
                auto found = index.find(job.name);
                if (found != index.end())
                {
                    indexBytes -= std::min(indexBytes, found->second.size);
                    index.erase(found);
                }
                break;
            }
        }
    }

    void Write(const std::string& name, const std::string& contents)
    {
        const double start = NowMilliseconds();
        const auto path = EntryPath(name);
        const auto temporary = std::filesystem::path(path.string() + ".tmp." + std::to_string(ProcessId()));

        bool wrote = false;
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (file)
            {
                file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
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
            std::lock_guard<std::mutex> lock(mutex);
            ++storeFailures;
            if (storeFailures == 1 && debug)
            {
                ShaderCacheDebugLog("[projectm shader cache] cannot write " + path.string() +
                                    (error ? ": " + error.message() : std::string()));
            }
            return;
        }

        // Read it back once. On Windows the first open of a new file waits for the
        // antivirus scan (measured at about 7 ms per entry), and that wait belongs on
        // this thread, not on the render thread's first hit. Later opens take 0.1 ms.
        {
            std::string readBack;
            bool damaged = false;
            ReadSmallFile(path, MaxEntryBytes, readBack, damaged);
        }

        auto& entry = index[name];
        indexBytes -= std::min(indexBytes, entry.size);
        entry.size = contents.size();
        entry.modified = std::filesystem::file_time_type::clock::now();
        indexBytes += entry.size;
        ++storesSinceScan;
        lastTouched[name] = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(mutex);
            storeMilliseconds += NowMilliseconds() - start;
            ++stores;
        }

        if (indexBytes > maxBytes || storesSinceScan >= StoresBetweenRescans)
        {
            ScanAndPrune();
        }
    }

    void Touch(const std::string& name)
    {
        const auto now = std::chrono::steady_clock::now();
        auto found = lastTouched.find(name);
        if (found != lastTouched.end() && now - found->second < TouchInterval)
        {
            return;
        }
        lastTouched[name] = now;

        const auto modified = std::filesystem::file_time_type::clock::now();
        std::error_code error;
        std::filesystem::last_write_time(EntryPath(name), modified, error);
        if (error)
        {
            // Gone (pruned by another process) or not writable: nothing to refresh.
            return;
        }
        auto entry = index.find(name);
        if (entry != index.end())
        {
            entry->second.modified = modified;
        }
        std::lock_guard<std::mutex> lock(mutex);
        ++touches;
    }

    /**
     * @brief Reads the directory, deletes stale temporary files and, if the entries
     *        are over the budget, deletes the least recently used ones.
     *
     * The modification time is the time of last use: a store sets it, and a hit
     * on a GLSL entry refreshes it through Touch(). A reject entry is never
     * touched, so its modification time is the time it was written, and it ages
     * out here like an unused entry if it is not removed at expiry first. Another
     * process pruning the same directory at
     * the same time only makes some removals find nothing to remove.
     */
    void ScanAndPrune()
    {
        const double start = NowMilliseconds();

        struct Entry
        {
            std::string name;
            std::filesystem::file_time_type modified;
            std::uintmax_t size{};
        };

        std::vector<Entry> entries;
        std::uintmax_t totalBytes = 0;
        const auto now = std::filesystem::file_time_type::clock::now();

        std::error_code error;
        std::filesystem::directory_iterator item(directory, error);
        const std::filesystem::directory_iterator end;
        for (; !error && item != end; item.increment(error))
        {
            const std::string name = item->path().filename().string();
            const bool isTemporary = name.find(".tmp") != std::string::npos;
            const bool isEntry = !isTemporary &&
                                 ((name.size() > 5 && name.compare(name.size() - 5, 5, ".glsl") == 0) ||
                                  (name.size() > 7 && name.compare(name.size() - 7, 7, ".reject") == 0));

            std::error_code entryError;
            if (isTemporary)
            {
                // Leftover from a process that died between write and rename.
                const auto modified = item->last_write_time(entryError);
                if (!entryError && now - modified > StaleTemporary)
                {
                    std::filesystem::remove(item->path(), entryError);
                }
                continue;
            }
            if (!isEntry)
            {
                continue;
            }

            Entry entry;
            entry.name = name;
            entry.modified = item->last_write_time(entryError);
            if (entryError)
            {
                continue;
            }
            entry.size = item->file_size(entryError);
            if (entryError)
            {
                continue;
            }
            totalBytes += entry.size;
            entries.push_back(std::move(entry));
        }

        std::uint64_t removed = 0;
        if (totalBytes > maxBytes)
        {
            std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
                return left.modified < right.modified;
            });

            const auto target = static_cast<std::uintmax_t>(static_cast<double>(maxBytes) * PruneTargetFraction);
            std::size_t at = 0;
            for (; at < entries.size() && totalBytes > target; ++at)
            {
                std::error_code removeError;
                std::filesystem::remove(EntryPath(entries[at].name), removeError);
                if (!removeError)
                {
                    // Removed here, or already removed by another process: gone either way.
                    totalBytes -= std::min(totalBytes, entries[at].size);
                    ++removed;
                    lastTouched.erase(entries[at].name);
                }
            }
            entries.erase(entries.begin(), entries.begin() + static_cast<std::ptrdiff_t>(at));
        }

        index.clear();
        for (auto& entry : entries)
        {
            index[entry.name] = IndexEntry{entry.size, entry.modified};
        }
        indexBytes = totalBytes;
        storesSinceScan = 0;

        const double elapsed = NowMilliseconds() - start;
        std::lock_guard<std::mutex> lock(mutex);
        ++scans;
        scanMilliseconds += elapsed;
        lastScanEntries = entries.size();
        lastScanBytes = totalBytes;
        entriesPruned += removed;
        if (debug && removed > 0)
        {
            ShaderCacheDebugLog("[projectm shader cache] pruned " + std::to_string(removed) + " least recently used entries, " +
                                std::to_string(entries.size()) + " entries and " + std::to_string(totalBytes) + " bytes kept");
        }
    }

    void PrintSummary()
    {
        if (!enabled || !debug)
        {
            return;
        }

        std::lock_guard<std::mutex> lock(mutex);
        const std::uint64_t lookups = hits + misses;
        const double hitRate = lookups == 0 ? 0.0 : (100.0 * static_cast<double>(hits) / static_cast<double>(lookups));
        const double averageMiss = misses == 0 ? 0.0 : transpileMilliseconds / static_cast<double>(misses);
        const double estimatedSaved = static_cast<double>(hits) * averageMiss - hitMilliseconds;

        char line[768];
        std::snprintf(line, sizeof(line),
                      "[projectm shader cache] summary: directory=%s lookups=%llu hits=%llu misses=%llu hit-rate=%.1f%% "
                      "unreadable=%llu store-failures=%llu compile-failures=%llu verify-mismatches=%llu "
                      "reject-hits=%llu rejects-stored=%llu rejects-expired=%llu reject-verify-mismatches=%llu pruned=%llu",
                      directory.c_str(),
                      static_cast<unsigned long long>(lookups),
                      static_cast<unsigned long long>(hits),
                      static_cast<unsigned long long>(misses),
                      hitRate,
                      static_cast<unsigned long long>(unreadable),
                      static_cast<unsigned long long>(storeFailures),
                      static_cast<unsigned long long>(compileFailures),
                      static_cast<unsigned long long>(verifyMismatches),
                      static_cast<unsigned long long>(rejectHits),
                      static_cast<unsigned long long>(rejectsStored),
                      static_cast<unsigned long long>(rejectsExpired),
                      static_cast<unsigned long long>(rejectVerifyMismatches),
                      static_cast<unsigned long long>(entriesPruned));
        ShaderCacheDebugLog(line);

        std::snprintf(line, sizeof(line),
                      "[projectm shader cache] timings: hit-read=%.1fms (max %.2fms) transpiled=%.1fms over %llu misses "
                      "(%.1fms each) estimated-saved=%.1fms",
                      hitMilliseconds,
                      maxHitMilliseconds,
                      transpileMilliseconds,
                      static_cast<unsigned long long>(misses),
                      averageMiss,
                      estimatedSaved);
        ShaderCacheDebugLog(line);

        std::snprintf(line, sizeof(line),
                      "[projectm shader cache] worker: stores=%llu (%.1fms) touches=%llu dropped=%llu scans=%llu (%.1fms) "
                      "startup-scan=%.1fms startup-entries=%llu startup-bytes=%llu entries=%llu bytes=%llu budget=%llu",
                      static_cast<unsigned long long>(stores),
                      storeMilliseconds,
                      static_cast<unsigned long long>(touches),
                      static_cast<unsigned long long>(droppedJobs),
                      static_cast<unsigned long long>(scans),
                      scanMilliseconds,
                      startupScanMilliseconds,
                      static_cast<unsigned long long>(startupEntries),
                      static_cast<unsigned long long>(startupBytes),
                      static_cast<unsigned long long>(lastScanEntries),
                      static_cast<unsigned long long>(lastScanBytes),
                      static_cast<unsigned long long>(maxBytes));
        ShaderCacheDebugLog(line);
    }

    /// Records one hit's read time. Called with `mutex` held.
    void RecordHitLocked(double milliseconds)
    {
        ++hits;
        hitMilliseconds += milliseconds;
        maxHitMilliseconds = std::max(maxHitMilliseconds, milliseconds);
    }

    /// The reject entry's name: the transpile key and the driver, hashed together.
    static auto RejectName(const std::string& key, const std::string& driver) -> std::string
    {
        Sha256 hash;
        UpdateString(hash, "projectm-preset-shader-reject");
        UpdateString(hash, key);
        UpdateString(hash, driver);
        return hash.FinalHex() + ".reject";
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
    if (m_impl->Shutdown())
    {
        delete m_impl;
    }
    // Otherwise a detached worker still uses it; the process is exiting anyway.
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
    const std::string name = key + ".glsl";

    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        if (auto pending = m_impl->PendingLocked(name))
        {
            // Stored by this process a moment ago, not yet on disk.
            glsl = *pending;
            m_impl->RecordHitLocked(NowMilliseconds() - start);
            return true;
        }
    }

    std::string contents;
    bool damaged = false;
    const bool read = ReadSmallFile(m_impl->EntryPath(name), MaxEntryBytes, contents, damaged);

    // Cheap sanity check: every generated preset shader has a main() function.
    // Anything else is a damaged entry, and a miss costs only one transpile.
    if (read && contents.find("void main()") == std::string::npos)
    {
        damaged = true;
    }

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!read || damaged)
    {
        if (damaged)
        {
            ++m_impl->unreadable;
        }
        ++m_impl->misses;
        return false;
    }

    glsl = std::move(contents);
    m_impl->RecordHitLocked(NowMilliseconds() - start);
    m_impl->RequestTouchLocked(name);
    return true;
}

void ShaderTranspileCache::Store(const std::string& key, const std::string& glsl)
{
    if (!m_impl->enabled || glsl.empty())
    {
        return;
    }

    Impl::Job job;
    job.kind = Impl::Job::Kind::Store;
    job.name = key + ".glsl";
    job.payload = std::make_shared<const std::string>(glsl);

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->EnqueueLocked(std::move(job));
}

void ShaderTranspileCache::Discard(const std::string& key)
{
    if (!m_impl->enabled)
    {
        return;
    }

    Impl::Job job;
    job.kind = Impl::Job::Kind::Discard;
    job.name = key + ".glsl";

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->EnqueueLocked(std::move(job));
}

auto ShaderTranspileCache::LookupRejected(const std::string& key, const std::string& driver, std::string& reason) -> bool
{
    reason.clear();
    if (!m_impl->enabled)
    {
        return false;
    }

    const std::string name = Impl::RejectName(key, driver);
    std::string contents;
    std::int64_t now = 0;
    {
        std::lock_guard<std::mutex> lock(m_impl->mutex);
        now = m_impl->WallSecondsLocked();
        if (auto pending = m_impl->PendingLocked(name))
        {
            contents = *pending;
        }
    }

    if (contents.empty())
    {
        bool damaged = false;
        if (!ReadSmallFile(m_impl->EntryPath(name), MaxRejectBytes, contents, damaged))
        {
            return false;
        }
    }

    // "<magic>\n<driver>\nwritten <unix seconds>\n<reason>\n": the driver line must
    // match exactly, so a different driver can never read another one's verdict.
    // The name already hashes the driver, so a file that fails these checks is this
    // shader's entry in an older format or damaged, never another driver's.
    const std::string header = std::string(RejectMagic) + "\n" + driver + "\n" + RejectWrittenPrefix;
    bool valid = contents.compare(0, header.size(), header) == 0;
    std::int64_t written = 0;
    std::size_t lineEnd = std::string::npos;
    if (valid)
    {
        lineEnd = contents.find('\n', header.size());
        const std::string date = contents.substr(header.size(), lineEnd == std::string::npos ? std::string::npos : lineEnd - header.size());
        char* end = nullptr;
        written = static_cast<std::int64_t>(std::strtoll(date.c_str(), &end, 10));
        valid = lineEnd != std::string::npos && !date.empty() && end != nullptr && *end == '\0';
    }

    // Expired 7 days after it was written, whether or not it was hit since; an
    // entry from the future means the clock was set back, and is not trusted.
    const std::int64_t age = now - written;
    const bool expired = valid && (age >= RejectLifetimeSeconds || -age > RejectFutureToleranceSeconds);

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (!valid || expired)
    {
        // Try the shader again; a fresh failure stores a new, dated entry.
        if (expired)
        {
            ++m_impl->rejectsExpired;
            if (m_impl->debug)
            {
                ShaderCacheDebugLog("[projectm shader cache] reject entry for key " + key.substr(0, 16) + " expired after " +
                                    std::to_string(age / 3600) + " h; trying the shader again");
            }
        }
        Impl::Job job;
        job.kind = Impl::Job::Kind::Discard;
        job.name = name;
        m_impl->EnqueueLocked(std::move(job));
        return false;
    }

    reason = FirstLine(contents.substr(lineEnd + 1));
    ++m_impl->rejectHits;
    // No touch: a hit must not extend the entry's life or its place in the LRU order.
    return true;
}

void ShaderTranspileCache::StoreRejected(const std::string& key, const std::string& driver, const std::string& reason)
{
    if (!m_impl->enabled)
    {
        return;
    }

    Impl::Job job;
    job.kind = Impl::Job::Kind::Store;
    job.name = Impl::RejectName(key, driver);

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    job.payload = std::make_shared<const std::string>(std::string(RejectMagic) + "\n" + driver + "\n" + RejectWrittenPrefix +
                                                      std::to_string(m_impl->WallSecondsLocked()) + "\n" + FirstLine(reason) + "\n");
    ++m_impl->rejectsStored;
    m_impl->EnqueueLocked(std::move(job));
}

void ShaderTranspileCache::DiscardRejected(const std::string& key, const std::string& driver)
{
    if (!m_impl->enabled)
    {
        return;
    }

    Impl::Job job;
    job.kind = Impl::Job::Kind::Discard;
    job.name = Impl::RejectName(key, driver);

    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->EnqueueLocked(std::move(job));
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

void ShaderTranspileCache::RecordRejectVerification(bool stillFails, const std::string& key)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    if (stillFails)
    {
        return;
    }

    ++m_impl->rejectVerifyMismatches;
    ShaderCacheDebugLog("[projectm shader cache] VERIFY MISMATCH: key " + key +
                        " is recorded as rejected on this driver but compiled; the reject entry was removed");
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

auto ShaderTranspileCache::Flush(int timeoutMilliseconds) -> bool
{
    std::unique_lock<std::mutex> lock(m_impl->mutex);
    if (!m_impl->enabled)
    {
        return true;
    }
    const auto done = [this] {
        return m_impl->queue.empty() && !m_impl->workerBusy && (m_impl->startupScanDone || !m_impl->workerRunning);
    };
    m_impl->idle.wait_for(lock, std::chrono::milliseconds(timeoutMilliseconds), done);
    return done();
}

auto ShaderTranspileCache::RejectEntryName(const std::string& key, const std::string& driver) -> std::string
{
    return Impl::RejectName(key, driver);
}

void ShaderTranspileCache::SetWallClockForTesting(WallClock clock)
{
    std::lock_guard<std::mutex> lock(m_impl->mutex);
    m_impl->wallClock = clock;
}

void ShaderCacheDebugLog(const std::string& line)
{
    std::fprintf(stderr, "%s\n", line.c_str());
    std::fflush(stderr);
}

} // namespace MilkdropPreset
} // namespace libprojectM
