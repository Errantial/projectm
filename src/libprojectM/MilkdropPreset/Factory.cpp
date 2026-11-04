#include "Factory.hpp"

#include "IdlePreset.hpp"
#include "MilkdropPreset.hpp"

namespace libprojectM {
namespace MilkdropPreset {

void Factory::PreparePresetData(const char* data)
{
    PreparedPreset::Prepare(data);
}

auto Factory::LoadPreparedPreset(const char* data) -> std::unique_ptr<::libprojectM::Preset>
{
    auto prepared = PreparedPreset::Take(data);
    if (!prepared)
    {
        return {};
    }
    return std::make_unique<MilkdropPreset>(std::move(prepared));
}

std::unique_ptr<::libprojectM::Preset> Factory::LoadPresetFromFile(const std::string& filename)
{
    std::string path;
    auto protocol = PresetFactory::Protocol(filename, path);
    if (protocol == "idle")
    {
        return IdlePresets::allocate();
    }
    else if (protocol == "" || protocol == "file")
    {
        return std::make_unique<MilkdropPreset>(path);
    }
    else
    {
        // ToDO: Throw unsupported protocol exception instead to provide more information.
        return nullptr;
    }
}

std::unique_ptr<Preset> Factory::LoadPresetFromStream(std::istream& data)
{
    return std::make_unique<MilkdropPreset>(data);
}

} // namespace MilkdropPreset
} // namespace libprojectM
