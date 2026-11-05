/**
 * @file PresetState.hpp
 *
 * Declares a class that hold the variable states for a single preset.
 */
#pragma once

#include "Constants.hpp"

#include "BlurTexture.hpp"

#include <Audio/FrameAudioData.hpp>

#include <Renderer/RenderContext.hpp>
#include <Renderer/Shader.hpp>
#include <Renderer/TextureSamplerDescriptor.hpp>

#include <projectm-eval.h>

#include <algorithm>
#include <iterator>
#include <memory>
#include <string>

namespace libprojectM {
namespace MilkdropPreset {

class PresetFileParser;

using BlendableFloat = float; //!< Currently a placeholder to mark blendable values.

/**
 * @brief Hold the current preset state and initial values.
 *
 * This is the base state class which is filled on preset load and updated between frames
 * to reflect the current state of rendering.
 */
class PresetState
{
public:
    /**
     * @brief reg00-reg99 of a prepared preset (see PreparedPreset), compiled against before the
     * preset exists and so kept on the heap.
     */
    struct GlobalRegisters {
        PRJM_EVAL_F values[100]{};
    };

    PresetState();

    /**
     * @brief Creates the state around expression memory that was filled ahead of time.
     * @param adoptedMemory The gmegabuf buffer. The state destroys it.
     * @param adoptedRegisters The reg00-reg99 variables the prepared code addresses.
     */
    PresetState(projectm_eval_mem_buffer adoptedMemory, std::unique_ptr<GlobalRegisters> adoptedRegisters);

    ~PresetState();

    /**
     * @brief Loads the initial values and code from the preset file.
     * @param parsedFile The file parser with the preset data.
     */
    void Initialize(PresetFileParser& parsedFile);

    /**
     * @brief Loads or compiles the generic shaders.
     * Call after setting renderContext.
     */
    void LoadShaders();

    BlendableFloat gammaAdj{2.0f};
    BlendableFloat videoEchoZoom{2.0f};
    BlendableFloat videoEchoAlpha{0.0f};
    BlendableFloat videoEchoAlphaOld{0.0f};
    int videoEchoOrientation{0};
    int videoEchoOrientationOld{0};

    BlendableFloat decay{0.98f};

    int waveMode{0};
    int oldWaveMode{-1};
    bool additiveWaves{false};
    BlendableFloat waveAlpha{0.8f};
    BlendableFloat waveScale{1.0f};
    BlendableFloat waveSmoothing{0.75f};
    bool waveDots{false};
    bool waveThick{false};
    BlendableFloat waveParam{0.0f};
    bool modWaveAlphaByvolume{false};
    BlendableFloat modWaveAlphaStart{0.75f};
    BlendableFloat modWaveAlphaEnd{0.95f};
    float warpAnimSpeed{1.0f};
    BlendableFloat warpScale{1.0f};
    BlendableFloat zoomExponent{1.0f};
    BlendableFloat shader{0.0f};
    bool maximizeWaveColor{true};
    bool texWrap{true};
    bool darkenCenter{false};
    bool redBlueStereo{false};
    bool brighten{false};
    bool darken{false};
    bool solarize{false};
    bool invert{false};

    BlendableFloat zoom{1.0f};
    BlendableFloat rot{0.0f};
    BlendableFloat rotCX{0.5f};
    BlendableFloat rotCY{0.5f};
    BlendableFloat xPush{0.0f};
    BlendableFloat yPush{0.0f};
    BlendableFloat warpAmount{1.0f};
    BlendableFloat stretchX{1.0f};
    BlendableFloat stretchY{1.0f};
    BlendableFloat waveR{1.0f};
    BlendableFloat waveG{1.0f};
    BlendableFloat waveB{1.0f};
    BlendableFloat waveX{0.5f};
    BlendableFloat waveY{0.5f};
    BlendableFloat outerBorderSize{0.01f};
    BlendableFloat outerBorderR{0.0f};
    BlendableFloat outerBorderG{0.0f};
    BlendableFloat outerBorderB{0.0f};
    BlendableFloat outerBorderA{0.0f};
    BlendableFloat innerBorderSize{0.01f};
    BlendableFloat innerBorderR{0.25f};
    BlendableFloat innerBorderG{0.25f};
    BlendableFloat innerBorderB{0.25f};
    BlendableFloat innerBorderA{0.0f};
    BlendableFloat mvX{12.0f};
    BlendableFloat mvY{9.0f};
    BlendableFloat mvDX{0.0f};
    BlendableFloat mvDY{0.0f};
    BlendableFloat mvL{0.9f};
    BlendableFloat mvR{1.0f};
    BlendableFloat mvG{1.0f};
    BlendableFloat mvB{1.0f};
    BlendableFloat mvA{0.0f};
    BlendableFloat blur1Min{0.0f};
    BlendableFloat blur2Min{0.0f};
    BlendableFloat blur3Min{0.0f};
    BlendableFloat blur1Max{1.0f};
    BlendableFloat blur2Max{1.0f};
    BlendableFloat blur3Max{1.0f};
    BlendableFloat blur1EdgeDarken{0.25f};

    int presetVersion{100};        //!< Value of MILKDROP_PRESET_VERSION in preset files.
    int warpShaderVersion{2};      //!< PSVERSION or PSVERSION_WARP.
    int compositeShaderVersion{2}; //!< PSVERSION or PSVERSION_COMP.

    std::array<float, 4> hueRandomOffsets; //!< Per-preset constant offsets for the hue animation

    projectm_eval_mem_buffer globalMemory{nullptr};  //!< gmegabuf data. Using per-frame buffers in projectM to reduce interference.
    double globalRegisters[100]{};                   //!< Global reg00-reg99 variables.
    std::array<double, QVarCount> frameQVariables{}; //!< Q variables after per-frame code evaluation.

    libprojectM::Audio::FrameAudioData audioData; //!< Holds audio/spectrum data and values for beat detection.
    Renderer::RenderContext renderContext;        //!< Current renderer state data like viewport size and generic shaders.

    std::string perFrameInitCode; //!< Preset init code, run once on load.
    std::string perFrameCode;     //!< Preset per-frame code, run once at the start of each frame.
    std::string perPixelCode;     //!< Preset per-pixel/per-vertex code, run once per warp mesh vertex.

    std::array<std::string, CustomWaveformCount> customWaveInitCode;     //!< Custom wave init code, run once on load.
    std::array<std::string, CustomWaveformCount> customWavePerFrameCode; //!< Custom wave per-frame code, run once after the per-frame code.
    std::array<std::string, CustomWaveformCount> customWavePerPointCode; //!< Custom wave per-point code, run once per waveform vertex.

    std::array<std::string, CustomShapeCount> customShapeInitCode;     //!< Custom shape init code, run once on load.
    std::array<std::string, CustomShapeCount> customShapePerFrameCode; //!< Custom shape per-frame code, run once per shape instance.

    std::string warpShader;      //!< Warp shader code.
    std::string compositeShader; //!< Composite shader code.

    std::weak_ptr<Renderer::Shader> untexturedShader; //!< Shader used to draw untextured primitives, e.g. waveforms.
    std::weak_ptr<Renderer::Shader> texturedShader;   //!< Shader used to draw textured primitives, e.g. textured shapes and the warp mesh.

    std::weak_ptr<Renderer::Texture> mainTexture; //!< A weak reference to the main texture in the preset framebuffer.
    BlurTexture blurTexture;                      //!< The blur textures used in this preset. Contents depend on the shader code using GetBlurX().

    std::map<int, Renderer::TextureSamplerDescriptor> randomTextureDescriptors; //!< Descriptors for random texture IDs. Should be the same across both warp and comp shaders.

    static const glm::mat4 orthogonalProjection;        //!< Projection matrix that transforms DirectX screen-space coordinates into the OpenGL coordinate frame.
    static const glm::mat4 orthogonalProjectionFlipped; //!< Projection matrix that transforms DirectX screen-space coordinates into the OpenGL coordinate frame.

    // Members added after the original ones, so every original member keeps its offset: a
    // custom waveform with a large "sep" reads samples before the start of audioData's arrays,
    // which lands on frameQVariables and globalRegisters, and must keep reading what it read.

    std::unique_ptr<GlobalRegisters> preparedRegisters; //!< A prepared preset's registers, which its code uses instead of globalRegisters.

    /**
     * @brief Copies a prepared preset's registers into globalRegisters, so the bytes before the
     * audio arrays hold the live register values, as they do for a preset loaded the usual way.
     * Call before code that reads the audio arrays.
     */
    void MirrorPreparedRegisters()
    {
        if (preparedRegisters)
        {
            std::copy(std::begin(preparedRegisters->values), std::end(preparedRegisters->values), std::begin(globalRegisters));
        }
    }
};

} // namespace MilkdropPreset
} // namespace libprojectM
