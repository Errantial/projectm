#include "CustomShape.hpp"

#include "PresetFileParser.hpp"

#include <Renderer/BlendMode.hpp>
#include <Renderer/TextureManager.hpp>

#include <array>
#include <vector>

namespace libprojectM {
namespace MilkdropPreset {

CustomShape::CustomShape(PresetState& presetState)
    : m_outlineMesh(Renderer::VertexBufferUsage::StreamDraw)
    , m_fillMesh(Renderer::VertexBufferUsage::StreamDraw, true, false)
    , m_presetState(presetState)
    , m_perFrameContext(presetState.globalMemory, &presetState.globalRegisters)
{
    m_outlineMesh.SetVertexCount(100);
    m_outlineMesh.SetRenderPrimitiveType(Renderer::Mesh::PrimitiveType::LineLoop);

    m_fillMesh.SetVertexCount(102);
    m_fillMesh.SetRenderPrimitiveType(Renderer::Mesh::PrimitiveType::TriangleFan);

    m_perFrameContext.RegisterBuiltinVariables();
}

void CustomShape::Initialize(PresetFileParser& parsedFile, int index)
{
    std::string const shapecodePrefix = "shapecode_" + std::to_string(index) + "_";

    m_index = index;
    m_enabled = parsedFile.GetBool(shapecodePrefix + "enabled", m_enabled);
    m_sides = parsedFile.GetInt(shapecodePrefix + "sides", m_sides);
    m_additive = parsedFile.GetBool(shapecodePrefix + "additive", m_additive);
    m_thickOutline = parsedFile.GetBool(shapecodePrefix + "thickOutline", m_thickOutline);
    m_textured = parsedFile.GetBool(shapecodePrefix + "textured", m_textured);
    m_instances = parsedFile.GetInt(shapecodePrefix + "num_inst", m_instances);
    m_x = parsedFile.GetFloat(shapecodePrefix + "x", m_x);
    m_y = parsedFile.GetFloat(shapecodePrefix + "y", m_y);
    m_radius = parsedFile.GetFloat(shapecodePrefix + "rad", m_radius);
    m_angle = parsedFile.GetFloat(shapecodePrefix + "ang", m_angle);
    m_tex_ang = parsedFile.GetFloat(shapecodePrefix + "tex_ang", m_tex_ang);
    m_tex_zoom = parsedFile.GetFloat(shapecodePrefix + "tex_zoom", m_tex_zoom);
    m_r = parsedFile.GetFloat(shapecodePrefix + "r", m_r);
    m_g = parsedFile.GetFloat(shapecodePrefix + "g", m_g);
    m_b = parsedFile.GetFloat(shapecodePrefix + "b", m_b);
    m_a = parsedFile.GetFloat(shapecodePrefix + "a", m_a);
    m_r2 = parsedFile.GetFloat(shapecodePrefix + "r2", m_r2);
    m_g2 = parsedFile.GetFloat(shapecodePrefix + "g2", m_g2);
    m_b2 = parsedFile.GetFloat(shapecodePrefix + "b2", m_b2);
    m_a2 = parsedFile.GetFloat(shapecodePrefix + "a2", m_a2);
    m_border_r = parsedFile.GetFloat(shapecodePrefix + "border_r", m_border_r);
    m_border_g = parsedFile.GetFloat(shapecodePrefix + "border_g", m_border_g);
    m_border_b = parsedFile.GetFloat(shapecodePrefix + "border_b", m_border_b);
    m_border_a = parsedFile.GetFloat(shapecodePrefix + "border_a", m_border_a);

    // projectM addition: texture name to use for rendering the shape
    m_image = parsedFile.GetString(shapecodePrefix + "image", "");
}

void CustomShape::CompileCodeAndRunInitExpressions()
{
    m_perFrameContext.LoadStateVariables(m_presetState, *this, 0);
    m_perFrameContext.EvaluateInitCode(m_presetState.customShapeInitCode[m_index], *this);

    for (int t = 0; t < TVarCount; t++)
    {
        m_tValuesAfterInitCode[t] = *m_perFrameContext.t_vars[t];
    }

    m_perFrameContext.CompilePerFrameCode(m_presetState.customShapePerFrameCode[m_index], *this);
}

void CustomShape::Draw()
{
    static constexpr float pi = 3.141592653589793f;

    if (!m_enabled)
    {
        return;
    }

    // Every instance's per-frame code runs first, then all of their geometry goes to the GPU in
    // one upload per buffer, and the instances are drawn from it in the same order, with the same
    // state, as one upload and draw per instance did before. Uploading once per instance made the
    // driver copy or wait on a buffer the previous draw still used, which cost far more than the
    // shapes' expressions in presets with hundreds of instances.
    auto& fillVertices = m_fillMesh.Vertices().Get();
    auto& fillColors = m_fillMesh.Colors().Get();
    auto& fillUVs = m_fillMesh.UVs().Get();
    auto& outlineVertices = m_outlineMesh.Vertices().Get();
    m_instanceDraws.clear();
    fillVertices.clear();
    fillColors.clear();
    fillUVs.clear();
    outlineVertices.clear();

    bool anyTextured{false};
    bool imageLookedUp{false};
    Renderer::TextureSamplerDescriptor imageDesc;

    for (int instance = 0; instance < m_instances; instance++)
    {
        m_perFrameContext.LoadStateVariables(m_presetState, *this, instance);
        m_perFrameContext.ExecutePerFrameCode();

        int sides = static_cast<int>(*m_perFrameContext.sides);
        if (sides < 3)
        {
            sides = 3;
        }
        if (sides > 100)
        {
            sides = 100;
        }

        InstanceDraw draw;
        draw.sides = sides;
        draw.additive = static_cast<int>(*m_perFrameContext.additive) != 0;
        draw.textured = static_cast<int>(*m_perFrameContext.textured) != 0;
        draw.fillFirst = static_cast<GLint>(fillVertices.size());

        const size_t first = fillVertices.size();
        fillVertices.resize(first + sides + 2);
        fillColors.resize(first + sides + 2);
        auto* vertexData = &fillVertices[first];
        auto* colorData = &fillColors[first];

        vertexData[0] = Renderer::Point(static_cast<float>(*m_perFrameContext.x * 2.0 - 1.0),
                                        static_cast<float>(*m_perFrameContext.y * -2.0 + 1.0));

        // x = f*255.0 & 0xFF = (f*255.0) % 256
        // f' = x/255.0 = f % (256/255)
        // 1.0 -> 255 (0xFF)
        // 2.0 -> 254 (0xFE)
        // -1.0 -> 0x01

        colorData[0] = Renderer::Color::Modulo(Renderer::Color(static_cast<float>(*m_perFrameContext.r),
                                                               static_cast<float>(*m_perFrameContext.g),
                                                               static_cast<float>(*m_perFrameContext.b),
                                                               static_cast<float>(*m_perFrameContext.a)));

        colorData[1] = Renderer::Color::Modulo(Renderer::Color(static_cast<float>(*m_perFrameContext.r2),
                                                               static_cast<float>(*m_perFrameContext.g2),
                                                               static_cast<float>(*m_perFrameContext.b2),
                                                               static_cast<float>(*m_perFrameContext.a2)));

        for (int i = 1; i < sides + 1; i++)
        {
            const float cornerProgress = static_cast<float>(i - 1) / static_cast<float>(sides);
            const float angle = cornerProgress * pi * 2.0f + static_cast<float>(*m_perFrameContext.ang) + pi * 0.25f;

            // Todo: There's still some issue with aspect ratio here, as everything gets squashed horizontally if Y > x.
            vertexData[i] = Renderer::Point(vertexData[0].X() + static_cast<float>(*m_perFrameContext.rad) * cosf(angle) * m_presetState.renderContext.aspectY,
                                            vertexData[0].Y() + static_cast<float>(*m_perFrameContext.rad) * sinf(angle));

            colorData[i] = colorData[1];
        }

        // Duplicate last vertex.
        vertexData[sides + 1] = vertexData[1];
        colorData[sides + 1] = colorData[1];

        if (draw.textured)
        {
            anyTextured = true;

            // Textured shape, either main texture or texture from "image" key
            auto textureAspectY = m_presetState.renderContext.aspectY;
            if (!m_image.empty())
            {
                if (!imageLookedUp)
                {
                    imageDesc = m_presetState.renderContext.textureManager->GetTexture(m_image);
                    imageLookedUp = true;
                }
                if (!imageDesc.Empty())
                {
                    textureAspectY = 1.0f;
                    draw.useImage = true;
                }
            }

            fillUVs.resize(fillVertices.size());
            auto* uvs = &fillUVs[first];

            uvs[0] = Renderer::TextureUV(0.5f, 0.5f);

            for (int i = 1; i < sides + 1; i++)
            {
                const float cornerProgress = static_cast<float>(i - 1) / static_cast<float>(sides);
                const float angle = cornerProgress * pi * 2.0f + static_cast<float>(*m_perFrameContext.tex_ang) + pi * 0.25f;

                uvs[i] = Renderer::TextureUV(0.5f + 0.5f * cosf(angle) / static_cast<float>(*m_perFrameContext.tex_zoom) * textureAspectY,
                                             1.0f - (0.5f - 0.5f * sinf(angle) / static_cast<float>(*m_perFrameContext.tex_zoom))); // Vertical flip required!
            }

            uvs[sides + 1] = uvs[1];
        }

        if (*m_perFrameContext.border_a > 0.0001f)
        {
            draw.border = true;
            draw.borderColor[0] = static_cast<float>(*m_perFrameContext.border_r);
            draw.borderColor[1] = static_cast<float>(*m_perFrameContext.border_g);
            draw.borderColor[2] = static_cast<float>(*m_perFrameContext.border_b);
            draw.borderColor[3] = static_cast<float>(*m_perFrameContext.border_a);
            draw.outlineFirst = static_cast<GLint>(outlineVertices.size());
            draw.iterations = m_thickOutline ? 4 : 1;

            // Need to use +/- 1.0 here instead of 2.0 used in Milkdrop to achieve the same rendering result.
            const auto incrementX = 1.0f / static_cast<float>(m_presetState.renderContext.viewportSizeX);
            const auto incrementY = 1.0f / static_cast<float>(m_presetState.renderContext.viewportSizeY);

            // If thick outline is used, draw the shape four times with slight offsets
            // (top left, top right, bottom right, bottom left), each pass moving the previous one.
            std::array<Renderer::Point, 100> points;
            for (int i = 0; i < sides; i++)
            {
                points[i] = fillVertices[first + i + 1];
            }
            for (auto iteration = 0; iteration < draw.iterations; iteration++)
            {
                switch (iteration)
                {
                    case 0:
                        break;

                    case 1:
                        for (auto j = 0; j < sides; j++)
                        {
                            points[j].SetX(points[j].X() + incrementX);
                        }
                        break;

                    case 2:
                        for (auto j = 0; j < sides; j++)
                        {
                            points[j].SetY(points[j].Y() + incrementY);
                        }
                        break;

                    case 3:
                        for (auto j = 0; j < sides; j++)
                        {
                            points[j].SetX(points[j].X() - incrementX);
                        }
                        break;
                }
                outlineVertices.insert(outlineVertices.end(), points.begin(), points.begin() + sides);
            }
        }

        m_instanceDraws.push_back(draw);
    }

    if (m_instanceDraws.empty())
    {
        return;
    }

    Renderer::BlendMode::SetBlendActive(true);

    // One upload per buffer for all instances.
    m_fillMesh.Bind();
    m_fillMesh.Vertices().Update();
    m_fillMesh.Colors().Update();
    if (anyTextured)
    {
        m_fillMesh.UVs().Update();
    }
    if (!outlineVertices.empty())
    {
        m_outlineMesh.Bind();
        m_outlineMesh.Vertices().Update();
        Renderer::VertexBuffer<Renderer::Color>::SetEnableAttributeArray(1, false);
        Renderer::VertexBuffer<Renderer::TextureUV>::SetEnableAttributeArray(2, false);
    }

    // The two shaders keep their uniforms between binds, and nothing else sets them while the
    // shape draws, so each is set once on first use instead of once per instance.
    bool texturedUniformsSet{false};
    bool untexturedUniformsSet{false};
    bool outlineUniformsSet{false};

    for (const auto& draw : m_instanceDraws)
    {
        // Additive Drawing or Overwrite
        Renderer::BlendMode::SetBlendFunction(Renderer::BlendMode::Function::SourceAlpha,
                                              draw.additive
                                                  ? Renderer::BlendMode::Function::One
                                                  : Renderer::BlendMode::Function::OneMinusSourceAlpha);

        m_fillMesh.Bind();
        Renderer::VertexBuffer<Renderer::Color>::SetEnableAttributeArray(1, true);
        Renderer::VertexBuffer<Renderer::TextureUV>::SetEnableAttributeArray(2, draw.textured);

        if (draw.textured)
        {
            auto shader = m_presetState.texturedShader.lock();
            shader->Bind();
            if (!texturedUniformsSet)
            {
                shader->SetUniformMat4x4("vertex_transformation", PresetState::orthogonalProjection);
                shader->SetUniformInt("texture_sampler", 0);
                texturedUniformsSet = true;
            }

            if (draw.useImage)
            {
                imageDesc.Bind(0, *shader);
            }
            else
            {
                // Main texture, also the fallback when the "image" texture was not found.
                assert(!m_presetState.mainTexture.expired());
                m_presetState.mainTexture.lock()->Bind(0);
            }

            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        }
        else
        {
            // Untextured (creates a color gradient: center=r/g/b/a to border=r2/b2/g2/a2)
            auto shader = m_presetState.untexturedShader.lock();
            shader->Bind();
            if (!untexturedUniformsSet)
            {
                shader->SetUniformMat4x4("vertex_transformation", PresetState::orthogonalProjection);
                untexturedUniformsSet = true;
            }
        }

        glDrawArrays(GL_TRIANGLE_FAN, draw.fillFirst, draw.sides + 2);
        Renderer::VertexArray::Unbind();

        glBindTexture(GL_TEXTURE_2D, 0);
        Renderer::Sampler::Unbind(0);

        if (draw.border)
        {
            auto shader = m_presetState.untexturedShader.lock();
            shader->Bind();
            if (!outlineUniformsSet)
            {
                shader->SetUniformMat4x4("vertex_transformation", PresetState::orthogonalProjection);
                shader->SetUniformFloat("vertex_point_size", 1.0f);
                outlineUniformsSet = true;
                untexturedUniformsSet = true;
            }

            glVertexAttrib4f(1, draw.borderColor[0], draw.borderColor[1], draw.borderColor[2], draw.borderColor[3]);
            glLineWidth(1);
#ifndef USE_GLES
            glEnable(GL_LINE_SMOOTH);
#endif

            m_outlineMesh.Bind();
            for (auto iteration = 0; iteration < draw.iterations; iteration++)
            {
                glDrawArrays(GL_LINE_LOOP, draw.outlineFirst + iteration * draw.sides, draw.sides);
            }
            Renderer::VertexArray::Unbind();
        }
    }

    Renderer::Mesh::Unbind();
    Renderer::Shader::Unbind();

#ifndef USE_GLES
    glDisable(GL_LINE_SMOOTH);
#endif
    Renderer::BlendMode::SetBlendActive(false);
}

} // namespace MilkdropPreset
} // namespace libprojectM
