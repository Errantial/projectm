#pragma once

#include "Constants.hpp"
#include "PresetFileParser.hpp"
#include "PresetState.hpp"

#include <projectm-eval.h>

#include <array>
#include <cstddef>
#include <memory>
#include <string>

namespace libprojectM {
namespace MilkdropPreset {

/**
 * @brief A preset's expression code, parsed and compiled ahead of its switch, on any thread.
 *
 * Parsing a preset, compiling its expressions and running the start of its per-frame init code
 * need no OpenGL. projectm_prepare_preset_data() does that work on the caller's thread, typically
 * a prefetch thread, while the previous preset plays. When the render thread later loads the same
 * data, MilkdropPreset takes the result over instead of doing the work itself, and only runs what
 * must see the moment of the switch: the rest of the init code, and the wave and shape init code.
 *
 * Only the leading top-level statements of the per-frame init code that name no built-in
 * variable other than q1-q32 (which are 0 when init code starts) and do not call rand() run
 * ahead. They see exactly what they would at the switch: the preset's own fresh variables,
 * megabuf, gmegabuf and registers. The rest is compiled on its own and runs on the render thread,
 * in order, after the built-in variables were loaded as before. The result is the same values in
 * the same order.
 *
 * A preset that does not parse or whose code does not compile is not kept: the render thread
 * then loads it the usual way and reports the same error it always did.
 */
class PreparedPreset
{
public:
    /**
     * @brief One compiled code block and the context it was compiled in.
     */
    struct Code {
        projectm_eval_context* context{nullptr};
        projectm_eval_code* code{nullptr};

        auto TakeContext() -> projectm_eval_context*;
        auto TakeCode() -> projectm_eval_code*;
    };

    PreparedPreset(const PreparedPreset&) = delete;
    auto operator=(const PreparedPreset&) -> PreparedPreset& = delete;

    ~PreparedPreset();

    /**
     * @brief Prepares preset data for a later load of the same data. Thread-safe.
     *
     * The result waits in a small process-wide store; the oldest one is dropped when it is full.
     * @param data The preset file contents.
     */
    static void Prepare(const char* data);

    /**
     * @brief Takes the prepared preset for exactly this data, if there is one. Thread-safe.
     * @param data The preset file contents.
     * @return The prepared preset, or nullptr.
     */
    static auto Take(const char* data) -> std::unique_ptr<PreparedPreset>;

    /**
     * @brief Hands over the gmegabuf buffer.
     */
    auto TakeGlobalMemory() -> projectm_eval_mem_buffer;

    PresetFileParser parser; //!< The parsed preset file.

    std::unique_ptr<PresetState::GlobalRegisters> registers; //!< reg00-reg99.

    Code perFrame;                              //!< Per-frame code. Its context also holds the init code.
    projectm_eval_code* perFrameInit{nullptr}; //!< Per-frame init code still to run, compiled in the per-frame context.
    bool perFrameInitRanAhead{false};          //!< Leading init statements already ran here.
    std::array<PRJM_EVAL_F, QVarCount> qAfterRanAhead{}; //!< q1-q32 as those statements left them.

    Code perPixel; //!< Per-pixel code.

    std::array<Code, CustomWaveformCount> wavePerFrame; //!< Custom wave per-frame code.
    std::array<Code, CustomWaveformCount> wavePerPoint; //!< Custom wave per-point code.
    std::array<Code, CustomShapeCount> shapePerFrame;   //!< Custom shape per-frame code.

private:
    PreparedPreset() = default;

    static auto Build(const char* data) -> std::unique_ptr<PreparedPreset>;

    std::string m_data;                                //!< The data this was prepared from.
    projectm_eval_mem_buffer m_globalMemory{nullptr}; //!< gmegabuf, until handed over.
};

} // namespace MilkdropPreset
} // namespace libprojectM
