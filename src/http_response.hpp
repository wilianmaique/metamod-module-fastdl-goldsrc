#pragma once
#include "assets.hpp"
#include <boost/beast/http/empty_body.hpp>
#include <boost/beast/http/message.hpp>

namespace fastdl {
namespace http = boost::beast::http;
using Request = http::request<http::empty_body>;

struct Reply {
    Reply(unsigned version, http::status status);
    http::response<http::empty_body> headers;
    std::unique_ptr<Asset> file;
    size_t offset = 0;
    size_t remaining = 0;
};

Reply prepare_reply(const Request& request, const std::filesystem::path& root, const Config& config);
}
