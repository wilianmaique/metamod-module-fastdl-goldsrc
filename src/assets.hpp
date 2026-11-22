#pragma once
#include "config.hpp"
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace fastdl {
// Each transfer owns one already-validated file, including for range reads.
class Asset {
public:
    using File = std::unique_ptr<std::FILE, decltype(&std::fclose)>;
    Asset(File file, size_t length, std::string validator);
    const size_t size;
    const std::string etag;
    size_t read(size_t offset, char* buffer, size_t length);
private:
    File file_;
    size_t position_ = 0;
};

std::unique_ptr<Asset> open_asset(const std::filesystem::path& root, std::string_view url_path,
                                  const ExtensionRules& rules);
}
