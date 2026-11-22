#pragma once
#include "config.hpp"
#include <memory>

namespace fastdl {
class HttpService {
public:
    HttpService(const std::filesystem::path& root, const Config& config);
    ~HttpService();
    HttpService(const HttpService&) = delete;
    HttpService& operator=(const HttpService&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
