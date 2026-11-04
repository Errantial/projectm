/**
 * @file ShaderTranspileCache.hpp
 * @brief On-disk cache for the HLSL to GLSL transpile output of preset shaders.
 *
 * Transpiling a Milkdrop preset shader is by far the most expensive part of loading
 * a preset: the bundled HLSL preprocessor and parser need 60 to 100 ms for the two
 * shaders of a single preset, while the driver compiles the resulting GLSL in a few
 * milliseconds once its own shader cache is warm. The generated GLSL is a pure
 * function of the preprocessed preset source, the sampler declarations and the
 * generator version, so it can be stored and reused for every later load of the same
 * preset.
 *
 * The cache is best-effort: every error (no directory, unreadable entry, garbage
 * contents, read-only file system) is treated as a cache miss, and a stored entry is
 * only ever used as the fragment shader source for a normal glCompileShader call, so
 * a wrong entry can never do more than fail compilation once and be replaced.
 */
#pragma once

#include <cstdint>
#include <string>

namespace libprojectM {
namespace MilkdropPreset {

/**
 * @brief Process-wide persistent cache of transpiled preset shader GLSL.
 *
 * Entries are one file per key, named "<sha256 hex>.glsl", containing nothing but
 * the generated GLSL source. The key hashes the cache format version, the shader
 * type, the GLSL generator version, the required blur level, the exact sampler and
 * texsize declaration block that will be inserted into the source, and the complete
 * preprocessed shader source. Any change to any input therefore invalidates the
 * entry, including changes to the preset, the static shader header and the
 * generator itself.
 *
 * A shader that this driver refused (or that could not be transpiled at all) is
 * remembered as a "<sha256 hex>.reject" entry, keyed on the same inputs plus the
 * GL vendor, renderer, version and GLSL version strings, so the next load goes
 * straight to the fallback without transpiling or compiling it again. A driver
 * update changes those strings and so invalidates every reject entry.
 *
 * Only reads happen on the caller's (render) thread. Every write, rename, mtime
 * update, directory scan and prune runs on one background thread per process,
 * which the calls below only hand work to. The thread runs while it has work and
 * stops after a short idle period, so an idle process holds no extra thread.
 *
 * The directory is bounded by a byte budget (256 MiB by default) and pruned least
 * recently used first: a hit refreshes the entry's modification time from the
 * background thread, at most once per entry every ten minutes, so the modification
 * time is the time of last use, also across processes that share the directory.
 *
 * Configuration, read once on first use:
 * - `PROJECTM_SHADER_CACHE=0` disables the cache.
 * - `PROJECTM_SHADER_CACHE_DIR=<path>` overrides the cache directory.
 * - `PROJECTM_SHADER_CACHE_MAX_MB=<n>` overrides the byte budget, for testing the
 *   pruning without filling 256 MiB.
 * - `PROJECTM_SHADER_CACHE_VERIFY=1` transpiles every hit again and logs a loud
 *   error if the cached source differs, and re-checks every reject entry (for
 *   testing the key, not for normal use).
 * - `PROJECTM_SHADER_CACHE_DEBUG=1` prints one line per lookup and a summary on exit.
 *
 * The default directory is `%LOCALAPPDATA%\projectm\shader-cache` on Windows and
 * `$XDG_CACHE_HOME/projectm/shader-cache` (or `~/.cache/projectm/shader-cache`) on
 * all other platforms.
 */
class ShaderTranspileCache
{
public:
    /**
     * @brief Bump this whenever the cache key or the payload format changes.
     * All existing entries are invalidated by a new value.
     */
    static constexpr int CacheFormatVersion = 1;

    /**
     * @brief The default byte budget of the cache directory.
     */
    static constexpr std::uint64_t DefaultMaxBytes = 256ull * 1024 * 1024;

    /**
     * @brief Returns the process-wide cache instance.
     */
    static auto Instance() -> ShaderTranspileCache&;

    /**
     * @brief Returns whether the cache is usable (not disabled, directory writable).
     */
    auto Enabled() const -> bool;

    /**
     * @brief Returns whether every hit is verified by a fresh transpile.
     */
    auto VerifyEnabled() const -> bool;

    /**
     * @brief Returns whether the cache prints its counters and timings.
     */
    auto DebugEnabled() const -> bool;

    /**
     * @brief Returns the directory entries are stored in. Empty if disabled.
     */
    auto Directory() const -> const std::string&;

    /**
     * @brief Returns why the cache is disabled, for debug output. Empty if enabled.
     */
    auto DisabledReason() const -> const std::string&;

    /**
     * @brief Computes the entry key for one transpile.
     * @param shaderType "warp" or "composite", as used in the parser messages.
     * @param glslVersion The M4::GLSLGenerator::Version the GLSL is generated for.
     * @param maxBlurLevel The highest blur level the shader requires.
     * @param declarations The exact sampler and texsize declaration block inserted
     *                     in front of the shader source.
     * @param program The complete preprocessed shader source handed to the
     *                transpiler, header and preset code included.
     * @return The lower-case hex SHA-256 of all inputs, length-prefixed.
     */
    auto Key(const std::string& shaderType,
             int glslVersion,
             int maxBlurLevel,
             const std::string& declarations,
             const std::string& program) const -> std::string;

    /**
     * @brief Reads a cached entry. Runs on the caller's thread; a hit also asks the
     *        background thread to refresh the entry's recency.
     * @param key The entry key, as returned by Key().
     * @param glsl Receives the stored GLSL source on a hit.
     * @return true if the entry existed and looked like GLSL source, false otherwise.
     */
    auto Lookup(const std::string& key, std::string& glsl) -> bool;

    /**
     * @brief Stores a successfully transpiled shader. Only queues the write.
     * Failures are counted and otherwise ignored.
     * @param key The entry key, as returned by Key().
     * @param glsl The generated GLSL source.
     */
    void Store(const std::string& key, const std::string& glsl);

    /**
     * @brief Removes an entry, e.g. after its GLSL failed to compile. Only queues it.
     * @param key The entry key to remove.
     */
    void Discard(const std::string& key);

    /**
     * @brief Returns whether this shader is known to fail on this driver.
     * @param key The entry key, as returned by Key().
     * @param driver The GL vendor, renderer, version and GLSL version strings.
     * @param reason Receives the stored reason on a hit.
     * @return true if a reject entry for this key and driver exists.
     */
    auto LookupRejected(const std::string& key, const std::string& driver, std::string& reason) -> bool;

    /**
     * @brief Remembers that this shader failed on this driver. Only queues the write.
     * @param key The entry key, as returned by Key().
     * @param driver The GL vendor, renderer, version and GLSL version strings.
     * @param reason Where it failed ("transpile" or "driver") and the first line of
     *               the error, for the log.
     */
    void StoreRejected(const std::string& key, const std::string& driver, const std::string& reason);

    /**
     * @brief Forgets a reject entry, e.g. when verify mode found that the shader
     *        compiles after all.
     */
    void DiscardRejected(const std::string& key, const std::string& driver);

    /**
     * @brief Records the outcome of a verification transpile.
     * @param matched true if the fresh transpile produced the cached source.
     * @param key The entry key that was verified.
     */
    void RecordVerification(bool matched, const std::string& key);

    /**
     * @brief Records the outcome of re-checking a reject entry in verify mode.
     * @param stillFails true if the shader failed again, as the entry says.
     * @param key The entry key that was verified.
     */
    void RecordRejectVerification(bool stillFails, const std::string& key);

    /**
     * @brief Records that compiling a cached entry failed and the shader was redone.
     * Logs the first occurrence.
     * @param key The entry key whose GLSL did not compile.
     */
    void RecordCompileFailure(const std::string& key);

    /**
     * @brief Records how long a complete transpile took, for the "time saved" estimate.
     * @param milliseconds The time spent preprocessing, parsing and generating.
     */
    void RecordTranspileTime(double milliseconds);

    /**
     * @brief Waits until the background thread has finished the work queued so far,
     *        or the timeout passed. For tests and tools; never needed for correctness.
     * @return true if the queue is empty.
     */
    auto Flush(int timeoutMilliseconds) -> bool;

private:
    ShaderTranspileCache();
    ~ShaderTranspileCache();

    ShaderTranspileCache(const ShaderTranspileCache&) = delete;
    auto operator=(const ShaderTranspileCache&) -> ShaderTranspileCache& = delete;

    struct Impl;
    Impl* m_impl;
};

/**
 * @brief Prints a line to stderr if the shader cache debug output is enabled.
 * @param line The line to print, without the trailing newline.
 */
void ShaderCacheDebugLog(const std::string& line);

} // namespace MilkdropPreset
} // namespace libprojectM
