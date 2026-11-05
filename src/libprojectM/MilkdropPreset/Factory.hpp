//
// C++ Interface: MilkdropPresetFactory
//
// Description:
//
//
// Author: Carmelo Piccione <carmelo.piccione@gmail.com>, (C) 2008
//
// Copyright: See COPYING file that comes with this distribution
//
//

#pragma once

#include <PresetFactory.hpp>

#include <memory>

namespace libprojectM {
namespace MilkdropPreset {

class Factory : public PresetFactory
{

public:
    std::unique_ptr<Preset> LoadPresetFromFile(const std::string& filename) override;

    std::unique_ptr<Preset> LoadPresetFromStream(std::istream& data) override;

    /**
     * @brief Parses and compiles preset data ahead of its load, on any thread (see PreparedPreset).
     * @param data The preset file contents.
     */
    static void PreparePresetData(const char* data);

    /**
     * @brief Creates the preset prepared from exactly this data, if PreparePresetData() did.
     * @param data The preset file contents.
     * @return The preset, or nullptr if this data was not prepared.
     */
    static auto LoadPreparedPreset(const char* data) -> std::unique_ptr<Preset>;

    std::string supportedExtensions() const override
    {
        return ".milk .prjm";
    }

};

} // namespace MilkdropPreset
} // namespace libprojectM
