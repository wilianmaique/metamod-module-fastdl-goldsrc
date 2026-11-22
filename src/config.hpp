#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace fastdl {
using ExtensionRules = std::map<std::string, std::vector<std::string>, std::less<>>;

struct Config {
    std::string bind = "127.0.0.1";
    int port = 8080;
    bool auto_set_downloadurl = true;
    std::string public_host;
    int public_port = 0;
    int workers = 4;
    int max_connections = 32;
    int cache_seconds = 0;
    // These 1.4 defaults also define the public folders. The empty path means root files only.
    ExtensionRules extensions{
        {"", {".wad"}},
        {"maps", {".bsp", ".res"}},
        {"models", {".mdl"}},
        {"sound", {".wav", ".mp3"}},
        {"sprites", {".spr", ".txt"}},
        {"overviews", {".txt", ".bmp", ".tga"}},
        {"gfx/env", {".tga", ".bmp"}},
    };
    std::string url() const;
};
Config read_config(const std::filesystem::path& file);
}
