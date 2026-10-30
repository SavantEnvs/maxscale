// libFuzzer harness for maxbase's INI configuration parser and its map/string conversions.
#include <cstdint>
#include <cstdlib>
#include <string>

#include <maxbase/ini.hh>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size > 16384)
    {
        return -1;
    }

    std::string text(reinterpret_cast<const char*>(data), size);

    auto arrays = mxb::ini::parse_config_text(text);
    auto maps = mxb::ini::parse_config_text_to_map(text);

    mxb::ini::substitute_env_vars(maps.config);
    std::string out = mxb::ini::config_map_to_string(maps.config);

    auto reparsed = mxb::ini::parse_config_text_to_map(out);
    (void)reparsed;
    (void)arrays;

    return 0;
}
