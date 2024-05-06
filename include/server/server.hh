#pragma once

#include <cstdint>
#include <expected>
#include <string_view>

#include "io.hh"
#include "result.hh"
#include "router.hh"

namespace http {
class Server {
    internal::Router router_;
    internal::Socket socket_;

public:
    void route(std::string_view target, Method method,
               const internal::RequestHandler& handler) noexcept;

    Result<void, std::string_view> bind(std::string_view address,
                                        uint16_t port) noexcept;

    void serve() const noexcept;
    void print_route_tree() const noexcept;
};
};  // namespace http
