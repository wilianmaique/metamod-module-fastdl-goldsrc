#include "assets.hpp"
#include <algorithm>
#include <cstdint>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

namespace fastdl {
namespace {
constexpr uint64_t max_size = 512ULL * 1024 * 1024;
using Parts = std::vector<std::string>;

std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}

bool valid_name(std::string_view s) {
    if (s.empty() || s.size() > 128 || s.front() == '.' || s.front() == ' ' ||
        s.back() == '.' || s.back() == ' ') return false;
    for (char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) continue;
        if (std::string_view("._- ()[]").find(c) == s.npos) return false;
    }
    auto stem = lower(std::string(s.substr(0, s.find('.'))));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    if (stem == "con" || stem == "nul" || stem == "prn" || stem == "aux") return false;
    return !(stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) &&
             stem[3] >= '0' && stem[3] <= '9');
}

bool published(std::string_view path, const ExtensionRules& rules) {
    auto slash = path.rfind('/');
    auto folder = slash == path.npos ? std::string_view{} : path.substr(0, slash);
    auto name = lower(std::string(slash == path.npos ? path : path.substr(slash + 1)));
    // A precompressed file is public only if its original name qualifies.
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".bz2") == 0) name.resize(name.size() - 4);
    auto dot = name.rfind('.');
    if (dot == name.npos || dot == 0) return false;
    auto ext = std::string_view(name).substr(dot);
    for (;;) {
        auto rule = rules.find(folder);
        if (rule != rules.end()) {
            const auto& allowed = rule->second;
            return std::find(allowed.begin(), allowed.end(), ext) != allowed.end();
        }
        // Only named directories are inherited. The root rule is never a fallback.
        slash = folder.rfind('/');
        if (slash == folder.npos) return false;
        folder = folder.substr(0, slash);
    }
}

std::string validator(uint64_t id, uint64_t size, uint64_t time, uint64_t change) {
    return "W/\"" + std::to_string(id) + "-" + std::to_string(size) + "-" +
           std::to_string(time) + "-" + std::to_string(change) + "\"";
}

// Convert the verified descriptor to a stream without reopening its pathname.
std::unique_ptr<Asset> adopt(int fd, size_t size, const std::string& etag) {
#ifdef _WIN32
    auto file = _fdopen(fd, "rb");
    if (!file) _close(fd);
#else
    auto file = fdopen(fd, "rb");
    if (!file) close(fd);
#endif
    if (!file) return {};
    Asset::File owner(file, std::fclose);
    return std::make_unique<Asset>(std::move(owner), size, etag);
}

#ifdef _WIN32
struct CloseHandleDeleter {
    void operator()(void* h) const { CloseHandle(h); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

Handle open_disk(const std::filesystem::path& path, bool directory) {
    DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT | (directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_FLAG_SEQUENTIAL_SCAN);
    HANDLE raw = CreateFileW(path.c_str(), directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
                             FILE_SHARE_READ | (directory ? 0 : FILE_SHARE_DELETE),
                             nullptr, OPEN_EXISTING, flags, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return {};
    Handle h(raw);
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(raw) != FILE_TYPE_DISK || !GetFileInformationByHandle(raw, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != directory) return {};
    return h;
}

std::wstring final_name(HANDLE h) {
    auto length = GetFinalPathNameByHandleW(h, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!length) return {};
    std::wstring name(length, L'\0');
    auto written = GetFinalPathNameByHandleW(h, name.data(), length, FILE_NAME_NORMALIZED);
    if (!written || written >= length) return {};
    name.resize(written);
    return name;
}

std::unique_ptr<Asset> open_verified(const std::filesystem::path& root, const Parts& parts) {
    // Pin directories while walking them. Check the final opened object as
    // well, including the real spelling of each component on case-sensitive NTFS.
    std::vector<Handle> directories;
    directories.push_back(open_disk(root, true));
    if (!directories.back()) return {};
    std::filesystem::path path(final_name(directories.back().get()));
    if (path.empty()) return {};
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        path /= parts[i];
        directories.push_back(open_disk(path, true));
        if (!directories.back()) return {};
    }
    path /= parts.back();
    auto h = open_disk(path, false);
    if (!h || final_name(h.get()) != path.native()) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h.get(), &info) || info.nNumberOfLinks != 1 ||
        info.nFileSizeHigh || info.nFileSizeLow > max_size) return {};
    auto join = [](DWORD hi, DWORD lo) { return (uint64_t(hi) << 32) | lo; };
    auto etag = validator(join(info.nFileIndexHigh, info.nFileIndexLow), info.nFileSizeLow,
                          join(info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime),
                          info.dwVolumeSerialNumber);
    int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h.get()), _O_RDONLY | _O_BINARY);
    if (fd < 0) return {};
    h.release();
    return adopt(fd, info.nFileSizeLow, etag);
}
#else
struct Descriptor {
    explicit Descriptor(int value) : fd(value) {}
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int fd;
    ~Descriptor() { if (fd >= 0) close(fd); }
};

std::unique_ptr<Asset> open_verified(const std::filesystem::path& root, const Parts& parts) {
    constexpr int dir_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    Descriptor dir{open(root.c_str(), dir_flags)};
    if (dir.fd < 0) return {};
    for (size_t i = 0; i < parts.size() - 1; ++i) {
        int next = openat(dir.fd, parts[i].c_str(), dir_flags);
        if (next < 0) return {};
        close(dir.fd);
        dir.fd = next;
    }
    Descriptor file{openat(dir.fd, parts.back().c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
    struct stat st{};
    if (file.fd < 0 || fstat(file.fd, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
        st.st_size < 0 || uint64_t(st.st_size) > max_size) return {};
    auto ns = [](timespec t) { return uint64_t(t.tv_sec) * 1000000000ULL + t.tv_nsec; };
    auto etag = validator(st.st_ino, st.st_size, ns(st.st_mtim), ns(st.st_ctim));
    int fd = file.fd;
    file.fd = -1;
    return adopt(fd, static_cast<size_t>(st.st_size), etag);
}
#endif
}

Asset::Asset(File file, size_t length, std::string tag)
    : size(length), etag(std::move(tag)), file_(std::move(file)) {}

size_t Asset::read(size_t offset, char* buffer, size_t length) {
    if (offset >= size || (offset != position_ && std::fseek(file_.get(), static_cast<long>(offset), SEEK_SET)))
        return 0;
    auto count = std::fread(buffer, 1, std::min(length, size - offset), file_.get());
    position_ = offset + count;
    return count;
}

std::unique_ptr<Asset> open_asset(const std::filesystem::path& root, std::string_view path,
                                  const ExtensionRules& rules) {
    if (path.size() > 256 || path.empty() || path.front() != '/') return {};
    path.remove_prefix(1);
    if (!published(path, rules)) return {};
    Parts parts;
    for (;;) {
        auto slash = path.find('/');
        auto part = path.substr(0, slash);
        if (!valid_name(part) || parts.size() >= 16) return {};
        parts.emplace_back(part);
        if (slash == path.npos) break;
        path.remove_prefix(slash + 1);
    }
    return open_verified(root, parts);
}
}
