#include "http_response.hpp"
#include <algorithm>
#include <charconv>
#include <iterator>
#include <optional>

namespace fastdl {
namespace {
int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string decode_path(std::string_view target) {
    target = target.substr(0, target.find('?'));
    std::string path;
    path.reserve(target.size());
    for (size_t i = 0; i < target.size(); ++i) {
        if (target[i] != '%') {
            path.push_back(target[i]);
            continue;
        }
        if (target.size() - i < 3) return {};
        int high = hex(target[i + 1]), low = hex(target[i + 2]);
        if (high < 0 || low < 0) return {};
        path.push_back(static_cast<char>(high * 16 + low));
        i += 2;
    }
    return path;
}

bool matches_etag(const Request& request, std::string_view tag) {
    auto fields = request.equal_range(http::field::if_none_match);
    for (auto it = fields.first; it != fields.second; ++it) {
        std::string_view list = it->value();
        while (!list.empty()) {
            auto comma = list.find(',');
            auto item = list.substr(0, comma);
            auto first = item.find_first_not_of(" \t");
            if (first != item.npos) {
                item = item.substr(first, item.find_last_not_of(" \t") - first + 1);
                if (item == "*" || item == tag || item == tag.substr(2)) return true;
            }
            if (comma == list.npos) break;
            list.remove_prefix(comma + 1);
        }
    }
    return false;
}

struct Range { size_t start, length; };

std::optional<Range> parse_range(std::string_view value, size_t size) {
    if (!size || !value.starts_with("bytes=")) return {};
    value.remove_prefix(6);
    auto dash = value.find('-');
    if (dash == value.npos) return {};
    auto number = [](std::string_view s, uint64_t& out) {
        auto end = s.data() + s.size();
        auto result = std::from_chars(s.data(), end, out);
        return result.ec == std::errc{} && result.ptr == end;
    };
    uint64_t first = 0, last = size - 1;
    if (dash == 0) {
        uint64_t suffix;
        if (!number(value.substr(1), suffix) || suffix == 0) return {};
        first = size - std::min<uint64_t>(suffix, size);
    } else {
        if (!number(value.substr(0, dash), first) || first >= size) return {};
        if (dash + 1 < value.size() && !number(value.substr(dash + 1), last)) return {};
        if (last < first) return {};
        last = std::min<uint64_t>(last, size - 1);
    }
    return Range{static_cast<size_t>(first), static_cast<size_t>(last - first + 1)};
}
}

Reply::Reply(unsigned version, http::status status) : headers(status, version) {
    headers.set(http::field::connection, "close");
    headers.set("X-Content-Type-Options", "nosniff");
    if (status != http::status::not_modified) headers.content_length(0);
}

Reply prepare_reply(const Request& req, const std::filesystem::path& root, const Config& config) {
    auto version = req.version();
    if (req.method() != http::verb::get && req.method() != http::verb::head) {
        Reply reply(version, http::status::method_not_allowed);
        reply.headers.set(http::field::allow, "GET, HEAD");
        return reply;
    }
    if (std::distance(req.begin(), req.end()) > 48 || req.target().empty() ||
        req.target().front() != '/' || req.target().size() > 512 ||
        (version == 11 && (req.count(http::field::host) != 1 || req[http::field::host].empty())) ||
        req.count(http::field::transfer_encoding) ||
        (req.count(http::field::content_length) && req[http::field::content_length] != "0"))
        return Reply(version, http::status::bad_request);

    auto file = open_asset(root, decode_path(req.target()), config.extensions);
    if (!file) return Reply(version, http::status::not_found);
    const bool cached = matches_etag(req, file->etag);
    Reply reply(version, cached ? http::status::not_modified : http::status::ok);
    reply.headers.set(http::field::etag, file->etag);
    reply.headers.set(http::field::cache_control,
                      "public, max-age=" + std::to_string(config.cache_seconds) + ", must-revalidate");
    reply.headers.set(http::field::accept_ranges, "bytes");
    if (cached) return reply;

    Range range{0, file->size};
    if (req.count(http::field::range) && !req.count(http::field::if_range)) {
        auto parsed = parse_range(req[http::field::range], file->size);
        if (!parsed || req.count(http::field::range) != 1) {
            Reply error(version, http::status::range_not_satisfiable);
            error.headers.set(http::field::content_range, "bytes */" + std::to_string(file->size));
            return error;
        }
        range = *parsed;
        reply.headers.result(http::status::partial_content);
        reply.headers.set(http::field::content_range, "bytes " + std::to_string(range.start) + "-" +
                          std::to_string(range.start + range.length - 1) + "/" + std::to_string(file->size));
    }
    reply.headers.set(http::field::content_type, "application/octet-stream");
    reply.headers.content_length(range.length);
    if (req.method() == http::verb::get && range.length) {
        reply.offset = range.start;
        reply.remaining = range.length;
        reply.file = std::move(file);
    }
    return reply;
}
}
