#include "PreparedPreset.hpp"

#include "InitCodeSplit.hpp"
#include "PerFrameContext.hpp"
#include "PerPixelContext.hpp"
#include "ShapePerFrameContext.hpp"
#include "WaveformPerFrameContext.hpp"
#include "WaveformPerPointContext.hpp"

#include <algorithm>
#include <cctype>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_set>

namespace libprojectM {
namespace MilkdropPreset {

namespace {

//! Prepared presets wait here until they are loaded. A prefetch prepares one ahead, so a few
//! entries are enough; the oldest goes when a preset was prepared and then skipped.
constexpr size_t StoreCapacity = 3;

std::mutex storeMutex;
std::deque<std::unique_ptr<PreparedPreset>> store;

auto Lower(std::string text) -> std::string
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

auto PreparedPreset::Code::TakeContext() -> projectm_eval_context*
{
    auto* taken = context;
    context = nullptr;
    return taken;
}

auto PreparedPreset::Code::TakeCode() -> projectm_eval_code*
{
    auto* taken = code;
    code = nullptr;
    return taken;
}

PreparedPreset::~PreparedPreset()
{
    auto destroy = [](Code& block) {
        if (block.code != nullptr)
        {
            projectm_eval_code_destroy(block.code);
        }
        if (block.context != nullptr)
        {
            projectm_eval_context_destroy(block.context);
        }
        block = {};
    };

    // Code goes before the context it was compiled in.
    if (perFrameInit != nullptr)
    {
        projectm_eval_code_destroy(perFrameInit);
    }
    destroy(perFrame);
    destroy(perPixel);
    for (auto& block : wavePerFrame)
    {
        destroy(block);
    }
    for (auto& block : wavePerPoint)
    {
        destroy(block);
    }
    for (auto& block : shapePerFrame)
    {
        destroy(block);
    }
    if (m_globalMemory != nullptr)
    {
        projectm_eval_memory_buffer_destroy(m_globalMemory);
    }
}

auto PreparedPreset::TakeGlobalMemory() -> projectm_eval_mem_buffer
{
    auto* taken = m_globalMemory;
    m_globalMemory = nullptr;
    return taken;
}

void PreparedPreset::Prepare(const char* data)
{
    if (data == nullptr)
    {
        return;
    }

    std::unique_ptr<PreparedPreset> prepared;
    try
    {
        prepared = Build(data);
    }
    catch (...)
    {
        // Anything that goes wrong here goes wrong again, and is reported, when the preset loads.
        return;
    }
    if (!prepared)
    {
        return;
    }

    std::unique_ptr<PreparedPreset> dropped;
    {
        std::lock_guard<std::mutex> lock(storeMutex);
        store.push_back(std::move(prepared));
        if (store.size() > StoreCapacity)
        {
            dropped = std::move(store.front());
            store.pop_front();
        }
    }
    // A dropped preset is freed outside the lock.
}

auto PreparedPreset::Take(const char* data) -> std::unique_ptr<PreparedPreset>
{
    if (data == nullptr)
    {
        return {};
    }

    std::lock_guard<std::mutex> lock(storeMutex);
    for (auto it = store.begin(); it != store.end(); ++it)
    {
        if ((*it)->m_data == data)
        {
            auto taken = std::move(*it);
            store.erase(it);
            return taken;
        }
    }
    return {};
}

auto PreparedPreset::Build(const char* data) -> std::unique_ptr<PreparedPreset>
{
    std::unique_ptr<PreparedPreset> prepared(new PreparedPreset());
    prepared->m_data = data;

    // Parsed exactly as projectm_load_preset_data() parses it.
    std::stringstream stream(prepared->m_data);
    if (!prepared->parser.Read(stream))
    {
        return {};
    }

    prepared->m_globalMemory = projectm_eval_memory_buffer_create();
    prepared->registers = std::make_unique<PresetState::GlobalRegisters>();
    auto* const memory = prepared->m_globalMemory;
    auto* const registers = &prepared->registers->values;

    // The same keys PresetState::Initialize() reads the code from.
    const auto perFrameInitCode = prepared->parser.GetCode("per_frame_init_");
    const auto perFrameCode = prepared->parser.GetCode("per_frame_");
    const auto perPixelCode = prepared->parser.GetCode("per_pixel_");

    auto compile = [](projectm_eval_context* context, const std::string& code, projectm_eval_code*& handle) {
        if (code.empty())
        {
            return true;
        }
        handle = projectm_eval_code_compile(context, code.c_str());
        return handle != nullptr;
    };

    {
        PerFrameContext context(memory, registers);
        context.RegisterBuiltinVariables();

        // The whole block must compile, as it must at the switch; a preset whose code does not
        // is left to fail there with its usual error.
        projectm_eval_code* init{nullptr};
        if (!compile(context.perFrameCodeContext, perFrameInitCode, init) ||
            !compile(context.perFrameCodeContext, perFrameCode, context.perFrameCodeHandle))
        {
            if (init != nullptr)
            {
                projectm_eval_code_destroy(init);
            }
            return {};
        }

        // Run the leading statements that cannot tell when they run: no built-in variable but
        // q1-q32 (which are 0 when init code starts, here too) and no rand(). The rest is
        // compiled on its own and runs at the switch.
        std::unordered_set<std::string> forbidden{"rand"};
        for (const char* name : context.BuiltinVariableNames())
        {
            forbidden.insert(Lower(name));
        }
        const size_t split = init != nullptr ? IndependentPrefixLength(perFrameInitCode, forbidden) : 0;
        if (split > 0)
        {
            const auto ahead = perFrameInitCode.substr(0, split);
            const auto rest = perFrameInitCode.substr(split);
            projectm_eval_code* aheadCode{nullptr};
            projectm_eval_code* restCode{nullptr};
            const bool restEmpty = rest.find_first_not_of(" \t\r\n;") == std::string::npos;
            if (compile(context.perFrameCodeContext, ahead, aheadCode) &&
                (restEmpty || compile(context.perFrameCodeContext, rest, restCode)))
            {
                projectm_eval_code_execute(aheadCode);
                projectm_eval_code_destroy(init);
                init = restCode;
                prepared->perFrameInitRanAhead = true;
            }
            else if (restCode != nullptr)
            {
                projectm_eval_code_destroy(restCode);
            }
            if (aheadCode != nullptr)
            {
                projectm_eval_code_destroy(aheadCode);
            }
        }
        prepared->perFrameInit = init;
        for (int q = 0; q < QVarCount; q++)
        {
            prepared->qAfterRanAhead[q] = *context.q_vars[q];
        }

        context.Release(prepared->perFrame.context, prepared->perFrame.code);
    }

    {
        PerPixelContext context(memory, registers);
        context.RegisterBuiltinVariables();
        if (!compile(context.perPixelCodeContext, perPixelCode, context.perPixelCodeHandle))
        {
            return {};
        }
        context.Release(prepared->perPixel.context, prepared->perPixel.code);
    }

    for (int index = 0; index < CustomWaveformCount; index++)
    {
        const std::string wavePrefix = "wave_" + std::to_string(index) + "_";

        WaveformPerFrameContext frameContext(memory, registers);
        frameContext.RegisterBuiltinVariables();
        if (!compile(frameContext.perFrameCodeContext, prepared->parser.GetCode(wavePrefix + "per_frame"), frameContext.perFrameCodeHandle))
        {
            return {};
        }
        frameContext.Release(prepared->wavePerFrame[index].context, prepared->wavePerFrame[index].code);

        WaveformPerPointContext pointContext(memory, registers);
        pointContext.RegisterBuiltinVariables();
        if (!compile(pointContext.perPointCodeContext, prepared->parser.GetCode(wavePrefix + "per_point"), pointContext.perPointCodeHandle))
        {
            return {};
        }
        pointContext.Release(prepared->wavePerPoint[index].context, prepared->wavePerPoint[index].code);
    }

    for (int index = 0; index < CustomShapeCount; index++)
    {
        const std::string shapePrefix = "shape_" + std::to_string(index) + "_";

        ShapePerFrameContext context(memory, registers);
        context.RegisterBuiltinVariables();
        if (!compile(context.perFrameCodeContext, prepared->parser.GetCode(shapePrefix + "per_frame"), context.perFrameCodeHandle))
        {
            return {};
        }
        context.Release(prepared->shapePerFrame[index].context, prepared->shapePerFrame[index].code);
    }

    return prepared;
}

} // namespace MilkdropPreset
} // namespace libprojectM
