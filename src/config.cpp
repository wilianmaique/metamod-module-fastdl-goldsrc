#include "config.hpp"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>

namespace fastdl {
namespace {
constexpr std::string_view extension_prefix = "extensions.";

std::string trim(std::string_view s) {
    auto first = s.find_first_not_of(" \t\r");
    if (first == s.npos) return {};
    return std::string(s.substr(first, s.find_last_not_of(" \t\r") - first + 1));
}

int number(std::string_view s, int low, int high) {
    int n = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), n);
    if (result.ec != std::errc{} || result.ptr != s.data() + s.size() || n < low || n > high)
        throw std::runtime_error("number outside allowed range");
    return n;
}

bool parse_bool(std::string_view value) {
    if (value == "true" || value == "1") return true;
    if (value == "false" || value == "0") return false;
    throw std::runtime_error("auto_set_downloadurl expects true, false, 1 or 0");
}

std::vector<std::string> parse_extensions(std::string_view value) {
    std::vector<std::string> extensions;
    if (!value.empty()) {
        for (;;) {
            auto comma = value.find(',');
            auto ext = trim(value.substr(0, comma));
            if (ext.size() < 2 || ext.front() != '.' ||
                ext.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789", 1) != ext.npos)
                throw std::runtime_error("expected comma-separated extensions such as .wav, .mp3");
            for (char& c : ext) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (ext == ".bz2") throw std::runtime_error(".bz2 uses the underlying file extension automatically");
            if (std::find(extensions.begin(), extensions.end(), ext) == extensions.end())
                extensions.push_back(std::move(ext));
            if (comma == value.npos) break;
            value.remove_prefix(comma + 1);
        }
    }
    return extensions;
}

bool valid_host(std::string_view host, bool ipv4) {
    int count = 0;
    for (;;) {
        auto dot = host.find('.');
        auto label = host.substr(0, dot);
        if (label.empty() || label.size() > 63) return false;
        if (ipv4) {
            if (label.size() > 1 && label.front() == '0') return false;
            number(label, 0, 255);
        } else {
            if (label.front() == '-' || label.back() == '-') return false;
            for (char c : label)
                if (!(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
                    !(c >= '0' && c <= '9') && c != '-') return false;
        }
        ++count;
        if (dot == host.npos) break;
        host.remove_prefix(dot + 1);
    }
    return !ipv4 || count == 4;
}
}

std::string Config::url() const {
    return "http://" + (public_host.empty() ? bind : public_host) + ":" +
           std::to_string(public_port ? public_port : port) + "/";
}

Config read_config(const std::filesystem::path& file) {
    std::ifstream input(file);
    if (!input) throw std::runtime_error("cannot open addons/fastdl/fastdl.ini");
    Config cfg;
    std::optional<std::vector<std::string>> global_extensions;
    bool has_legacy_rules = false;
    std::set<std::string> seen;
    std::string line;
    int lineno = 0;
    while (std::getline(input, line)) {
        ++lineno;
        if (lineno == 1 && line.compare(0, 3, "\xef\xbb\xbf") == 0) line.erase(0, 3);
        line = trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        try {
            auto equal = line.find('=');
            if (equal == line.npos) throw std::runtime_error("expected key = value");
            auto key = trim(line.substr(0, equal));
            auto value = trim(line.substr(equal + 1));
            if (!seen.insert(key).second) throw std::runtime_error("duplicate key " + key);
            if (key == "bind") cfg.bind = value;
            else if (key == "port") cfg.port = number(value, 1, 65535);
            else if (key == "auto_set_downloadurl") cfg.auto_set_downloadurl = parse_bool(value);
            else if (key == "public_host") cfg.public_host = value;
            else if (key == "public_port") cfg.public_port = number(value, 0, 65535);
            else if (key == "workers") cfg.workers = number(value, 1, 32);
            else if (key == "max_connections") cfg.max_connections = number(value, 1, 256);
            else if (key == "cache_seconds") cfg.cache_seconds = number(value, 0, 86400);
            else if (key == "extensions_files") global_extensions = parse_extensions(value);
            else if (key.starts_with(extension_prefix)) {
                auto folder = std::string_view(key).substr(extension_prefix.size());
                if (folder.empty()) throw std::runtime_error("missing folder after extensions.");
                auto rule = cfg.extensions.find(folder == "root" ? std::string_view{} : folder);
                if (rule == cfg.extensions.end()) throw std::runtime_error("unknown download folder " + std::string(folder));
                rule->second = parse_extensions(value);
                has_legacy_rules = true;
            } else throw std::runtime_error("unknown key " + key);
        } catch (const std::exception& error) {
            throw std::runtime_error("fastdl.ini line " + std::to_string(lineno) + ": " + error.what());
        }
    }
    if (!input.eof()) throw std::runtime_error("failed reading fastdl.ini");
    // An old per-folder INI keeps its exact permissions until the admin sets the global key.
    if (!global_extensions && !has_legacy_rules) {
        std::set<std::string> defaults;
        for (const auto& rule : cfg.extensions) defaults.insert(rule.second.begin(), rule.second.end());
        global_extensions.emplace(defaults.begin(), defaults.end());
    }
    if (global_extensions) {
        for (auto& rule : cfg.extensions) rule.second = *global_extensions;
    }
    if (!valid_host(cfg.bind, true)) throw std::runtime_error("bind must be a numeric IPv4 address");
    if (!cfg.auto_set_downloadurl) return cfg;
    const auto& host = cfg.public_host.empty() ? cfg.bind : cfg.public_host;
    bool numeric = host.find_first_not_of("0123456789.") == host.npos;
    if (!valid_host(host, numeric) || host == "0.0.0.0")
        throw std::runtime_error("configure public_host with a reachable IP or DNS name");
    if (cfg.url().size() > 128) throw std::runtime_error("sv_downloadurl exceeds 128 bytes");
    return cfg;
}
}
