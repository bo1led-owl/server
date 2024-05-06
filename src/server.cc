#include "waxwing/server.hh"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "concurrency.hh"
#include "waxwing/io.hh"
#include "waxwing/request.hh"
#include "waxwing/response.hh"
#include "waxwing/router.hh"
#include "waxwing/types.hh"
#include "str_split.hh"
#include "str_util.hh"

constexpr size_t HEADERS_BUFFER_SIZE = 2048;  // 2 Kb

namespace waxwing {
using namespace internal;

namespace {
std::pair<std::string, std::string> parse_header(const std::string_view s) {
    const size_t colon_pos = s.find(':');

    return {str_util::to_lower(str_util::trim(s.substr(0, colon_pos))),
            std::string{str_util::trim(
                s.substr(colon_pos + 1, s.size() - colon_pos - 1))}};
}

void concat_header(std::string& buf, const std::string_view key,
                   const std::string_view value) {
    buf += str_util::trim(key);
    buf += ": ";
    buf += str_util::trim(value);
    buf += "\r\n";
}

void concat_headers(std::string& buf, const Headers& headers) {
    for (const auto& [key, value] : headers) {
        concat_header(buf, key, value);
    }
}

Result<Request, std::string_view> read_request(const Connection& conn) {
    std::string buf;
    conn.recv(buf, HEADERS_BUFFER_SIZE);

    const auto line_split = str_util::split(buf, "\r\n");
    auto line_iter = line_split.begin();

    if (line_iter == line_split.end()) {
        return Error{"No request line"};
    }

    const auto token_split = str_util::split(*line_iter, ' ');
    auto token_iter = token_split.begin();

    auto get_tok = [&token_iter,
                    &token_split]() -> std::optional<std::string_view> {
        std::optional<std::string_view> result = std::nullopt;

        if (token_iter != token_split.end()) {
            result = *token_iter;
        }

        token_iter++;
        return result;
    };

    std::optional<Method> method = and_then(
        get_tok(), [](const std::string_view s) { return parse_method(s); });
    std::optional<std::string_view> target = get_tok();
    const std::optional<std::string_view> http_version = get_tok();

    if (!method || !target || !http_version) {
        return Error{"Error parsing request line"};
    }

    if (!target->starts_with('/')) {
        target = std::move(std::string("/").append(target.value()));
    }

    Headers headers;

    std::optional<size_t> content_length = std::nullopt;
    for (const std::string_view line :
         std::ranges::subrange(line_iter, line_split.end())) {
        if (line.empty()) {
            // request body starts here, breaking
            break;
        }
        const std::pair<std::string, std::string> header = parse_header(line);
        headers.insert_or_assign(std::string{header.first},
                                 std::string{header.second});

        if (header.first == "content-length") {
            size_t len;
            const auto [_, ec] = std::from_chars(
                header.second.begin().base(), header.second.end().base(), len);

            if (ec != std::errc{}) {
                return Error{"Error parsing `Content-Length`"};
            }

            content_length = len;
        }
    };

    std::string body{line_iter.remaining()};

    if (content_length.has_value()) {
        // content-length header is present
        body.reserve(body.size() + content_length.value());
        while (content_length > body.size()) {
            conn.recv(body, content_length.value() - body.size());
        }
    }

    return Request{method.value(), target.value(), std::move(headers),
                   std::move(body)};
}

void send_response(const Connection& conn, Response& resp) {
    std::string buf;

    buf += "HTTP/1.1 ";
    buf += format_status(resp.status());
    buf += "\r\n";

    Headers& headers = resp.headers();
    headers["Connection"] = "Close";

    const std::optional<Response::Body>& body_opt = resp.body();
    if (body_opt.has_value()) {
        headers["Content-Type"] = body_opt->type;

        const std::string content_length =
            std::to_string(body_opt->data.size());
        headers["Content-Length"] = content_length;
    }

    // push all of the headers into buf
    concat_headers(buf, headers);

    // empty line is required even if the body is empty
    buf += "\r\n";

    if (body_opt.has_value()) {
        buf += body_opt->data;
    }

    conn.send(buf);
}

Result<void, std::string_view> handle_connection(const Router& router,
                                                 Connection connection) {
    auto req_res = read_request(connection);
    if (!req_res) {
        return Error{req_res.error()};
    }

    Request& req = req_res.value();

    std::pair<RequestHandler, Params> route =
        router.route(req.target(), req.method());

    req.set_params(route.second);
    std::unique_ptr<Response> resp = route.first(req);
    send_response(connection, *resp);

    return {};
}
}  // namespace

void Server::route(const Method method, const std::string_view target,
                   const RequestHandler& handler) noexcept {
    router_.add_route(target, method, handler);
}

void Server::print_route_tree() const noexcept {
    router_.print_tree();
}

Result<void, std::string_view> Server::bind(const std::string_view address,
                                            const uint16_t port) noexcept {
    auto sock_res = Socket::create(address, port);
    if (!sock_res) {
        return Error{sock_res.error()};
    }
    socket_ = std::move(sock_res.value());

    return {};
}

void Server::serve() const noexcept {
    concurrency::ThreadPool thrd_pool;

    for (;;) {
        Connection connection = socket_.accept();
        if (connection.is_valid()) {
            thrd_pool.async([this, conn = std::move(connection)]() mutable {
                [[maybe_unused]] Result<void, std::string_view> result =
                    handle_connection(router_, std::move(conn));
            });
        }
    }
}
}
