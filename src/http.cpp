#include "http.hpp"
#include "http_response.hpp"
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/write.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace fastdl {
namespace {
namespace net = boost::asio;
namespace beast = boost::beast;
using tcp = net::ip::tcp;
using boost::system::error_code;
using namespace std::chrono_literals;
using net::use_awaitable;
constexpr size_t block_size = 64 * 1024;
constexpr size_t header_limit = 8192;
constexpr auto write_timeout = 30s;

void run_background(net::io_context& context) {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#else
    nice(5);
#endif
    context.run();
}
}

struct HttpService::Impl {
    const std::filesystem::path root;
    const Config config;
    net::io_context network{1};
    net::io_context disk;
    net::executor_work_guard<net::io_context::executor_type> disk_work{disk.get_executor()};
    tcp::acceptor acceptor{network};
    net::steady_timer admission{network};
    net::steady_timer shutdown_signal{network, net::steady_timer::time_point::max()};
    std::vector<std::shared_ptr<beast::tcp_stream>> active;
    std::vector<std::thread> workers;
    std::thread listener;
    std::atomic<bool> stopping{false};

    Impl(const std::filesystem::path& path, const Config& cfg)
        : root(std::filesystem::canonical(path)), config(cfg) {}

    void start() {
        active.reserve(config.max_connections);
        workers.reserve(config.workers);
        tcp::endpoint endpoint(net::ip::make_address_v4(config.bind),
                               static_cast<unsigned short>(config.port));
        acceptor.open(endpoint.protocol());
#ifdef _WIN32
        const int exclusive = 1;
        if (setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) == SOCKET_ERROR)
            throw std::system_error(WSAGetLastError(), std::system_category(), "SO_EXCLUSIVEADDRUSE");
#else
        acceptor.set_option(tcp::acceptor::reuse_address(true));
#endif
        acceptor.bind(endpoint);
        acceptor.listen(128);
        for (int i = 0; i < config.workers; ++i) workers.emplace_back(run_background, std::ref(disk));
        // Armed before the reactor starts. Its only subsequent access is the
        // cancellation from the unloading thread, which requires no posting allocation.
        shutdown_signal.async_wait([this](error_code) { close_all(); });
        net::co_spawn(network, listen(), [this](std::exception_ptr error) {
            if (error) close_all();
        });
        listener = std::thread(run_background, std::ref(network));
    }

    void close_all() {
        stopping = true;
        error_code ignored;
        acceptor.close(ignored);
        admission.cancel();
        for (auto& stream : active) stream->close();
    }

    ~Impl() {
        if (listener.joinable()) {
            shutdown_signal.cancel();
            listener.join();
        }
        // The network coroutines have received their last disk completions.
        // This also joins partial pools after a startup failure.
        disk_work.reset();
        for (auto& thread : workers) thread.join();
    }

    // Only disk workers enter these functions. The waiting network coroutine
    // owns the request, file and buffer until the worker returns.
    net::awaitable<void> prepare(const Request& req, Reply& result) {
        if (!stopping) result = prepare_reply(req, root, config);
        co_return;
    }

    net::awaitable<size_t> read_block(Asset& file, size_t offset, char* buffer, size_t length) {
        if (stopping) co_return 0;
        co_return file.read(offset, buffer, length);
    }

    void start_session(tcp::socket socket) {
        auto stream = std::make_shared<beast::tcp_stream>(std::move(socket));
        active.push_back(stream);
        try {
            net::co_spawn(network, serve(*stream), [this, stream](std::exception_ptr) {
                stream->close();
                std::erase(active, stream);
                if (!stopping && active.size() + 1 == static_cast<size_t>(config.max_connections))
                    admission.cancel_one();
            });
        } catch (...) {
            active.pop_back();
            throw;
        }
    }

    net::awaitable<void> listen() {
        while (!stopping) {
            error_code ec;
            if (active.size() >= static_cast<size_t>(config.max_connections)) {
                admission.expires_at(net::steady_timer::time_point::max());
                co_await admission.async_wait(net::redirect_error(use_awaitable, ec));
                continue;
            }
            auto socket = co_await acceptor.async_accept(net::redirect_error(use_awaitable, ec));
            if (stopping) co_return;
            if (!ec) {
                try {
                    start_session(std::move(socket));
                    continue;
                } catch (const std::bad_alloc&) {
                    // Back off on this admission while existing transfers continue.
                }
            }
            // Avoid spinning when the OS temporarily has no sockets available.
            admission.expires_after(100ms);
            co_await admission.async_wait(net::redirect_error(use_awaitable, ec));
        }
    }

    net::awaitable<void> serve(beast::tcp_stream& stream) {
        Reply reply(11, http::status::bad_request);
        {
            http::request_parser<http::empty_body> parser;
            parser.header_limit(header_limit);
            beast::flat_buffer input(header_limit);
            error_code ec;
            stream.expires_after(4s);
            co_await http::async_read_header(stream, input, parser, net::redirect_error(use_awaitable, ec));
            if (stopping || !stream.socket().is_open()) co_return;
            if (!ec) {
                co_await net::co_spawn(disk, prepare(parser.get(), reply), use_awaitable);
            } else if (ec == http::error::end_of_stream || ec == net::error::connection_reset) {
                co_return;
            }
        }
        if (stopping) co_return;
        {
            http::response_serializer<http::empty_body> serializer(reply.headers);
            stream.expires_after(write_timeout);
            co_await http::async_write_header(stream, serializer, use_awaitable);
        }
        if (!reply.remaining) co_return;
        auto buffer = std::make_unique_for_overwrite<char[]>(block_size);
        while (reply.remaining && !stopping) {
            auto count = co_await net::co_spawn(disk, read_block(*reply.file, reply.offset, buffer.get(),
                                                std::min(block_size, reply.remaining)), use_awaitable);
            if (!count || stopping) co_return;
            size_t sent = 0;
            while (sent < count) {
                stream.expires_after(write_timeout);
                auto written = co_await stream.async_write_some(net::buffer(buffer.get() + sent, count - sent),
                                                                use_awaitable);
                if (!written) co_return;
                sent += written;
            }
            reply.offset += count;
            reply.remaining -= count;
        }
    }
};

HttpService::HttpService(const std::filesystem::path& root, const Config& config)
    : impl_(std::make_unique<Impl>(root, config)) { impl_->start(); }
HttpService::~HttpService() = default;
}
