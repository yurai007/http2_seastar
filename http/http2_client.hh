/*
 * Copyright (C) 2018 ScyllaDB
 */

/*
 * This file is open source software, licensed to you under the terms
 * of the Apache License, Version 2.0 (the "License").  See the NOTICE file
 * distributed with this work for additional information regarding copyright
 * ownership.  You may not use this file except in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#pragma once

#include "core/reactor.hh"
#include "core/sstring.hh"
#include "core/app-template.hh"
#include "core/circular_buffer.hh"
#include "core/distributed.hh"
#include "core/queue.hh"
#include "core/future-util.hh"
#include <algorithm>
#include <vector>
#include "http/http2_connection.hh"
#include "net/tls.hh"

namespace seastar {
namespace httpd2 {

class http_client {
public:
    future<> connect(const uint16_t connections, ipv4_addr server_addr, bool with_tls) {
        if (!with_tls) {
            for (auto i = connections; i > 0u; i--) {
                engine().net().connect(make_ipv4_address(server_addr))
                        .then([this] (connected_socket fd) {
                    _sockets.push_back(std::move(fd));
                    _conn_connected.signal();
                }).or_terminate();
            }
            return _conn_connected.wait(connections);
        } else {
            constexpr auto hardcoded_pem = "/home/yurai/seastar/scylla/seastar/tests/catest.pem";
            auto builder = make_lw_shared<tls::credentials_builder>();
            return builder->set_x509_trust_file(hardcoded_pem, tls::x509_crt_format::PEM)
                        .then([this, connections, builder, server_addr = std::move(server_addr)]() {
                            auto creds = builder->build_certificate_credentials();
                                for (auto i = connections; i > 0u; i--) {
                                    tls::connect(creds, server_addr)
                                        .then([this] (connected_socket fd) {
                                            _sockets.push_back(std::move(fd));
                                            _conn_connected.signal();
                                    }).or_terminate();
                                }
                            return _conn_connected.wait(connections);
                });
        }
    }

    future<> send_burst(unsigned requests, lw_shared_ptr<request> req, http2_connection<session_t::client> *conn) {
        for (auto i = requests; i > 0; i--) {
            auto rv = conn->submit_request(req);
            if (rv < 0) {
                _failed_requests++;
                throw nghttp2_exception("submit_request", rv);
            }
        }
        return conn->process_internal(false);
    }

    future<> run(request *req, const uint16_t reqs) {
        fmt::print("established tcp connections\n");
        ipv4_addr server_addr = {"127.0.0.1", 3000u};
        _common_reqs = make_lw_shared<request>(*req);
        _common_reqs->done();
        for (auto &&socket : _sockets) {
            auto conn = new http2_connection<session_t::client>(&_routes, std::move(socket), std::move(make_ipv4_address(server_addr)));
            send_burst(reqs, _common_reqs, conn)
                    .then_wrapped([this, conn] (auto&& f) {
                        _conn_finished.signal();
                        conn->shutdown();
                        delete conn;
                        try {
                            f.get();
                        } catch (const std::exception& exception) {
                            fmt::print("http request error: {}\n", exception.what());
                        }
                });
        }
        return _conn_finished.wait(_sockets.size());
    }

    future<> stop() { return make_ready_future(); }
    future<uint64_t> responses() { return make_ready_future<uint64_t>(_responses); }
    routes _routes;
    uint64_t _responses{0}, _failed_requests {0};
private:
    semaphore _conn_connected{0}, _conn_finished{0};
    std::vector<connected_socket> _sockets;
    lw_shared_ptr<request> _common_reqs;
};

}
}
