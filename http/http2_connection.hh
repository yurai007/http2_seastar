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

#include "http2_file_handler.hh"
#include "http2_request_response.hh"
#include "core/iostream.hh"
#include "http/routes.hh"
#include "net/api.hh"
#include "core/future-util.hh"
#include "core/temporary_buffer.hh"
#include "core/sstring.hh"
#include "core/iostream.hh"
#include "core/app-template.hh"
#include "core/distributed.hh"
#include "net/socket_defs.hh"
#include "net/api.hh"
#include "net/tls.hh"
#include <nghttp2/nghttp2.h>
#include <boost/intrusive/list.hpp>
#include <optional>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <array>
#include <vector>
#include <stdexcept>

namespace seastar {
namespace httpd {

class session : public boost::intrusive::list_base_hook<> {
public:
    virtual future<> process() = 0;
    virtual void shutdown() = 0;
    virtual output_stream<char>& out() = 0;
    virtual ~session() = default;
};
}

static sstring http_date() {
    auto t = ::time(nullptr);
    struct tm tm;
    gmtime_r(&t, &tm);
    char tmp[100];
    strftime(tmp, sizeof(tmp), "%d %b %Y %H:%M:%S GMT", &tm);
    return tmp;
}
}

namespace seastar {
namespace httpd2 {

using user_callback = std::function<
                        future<
                            std::tuple<
                                lw_shared_ptr<request>, std::unique_ptr<response>>>
    (lw_shared_ptr<request>, std::unique_ptr<response>)>;
using client_callback = std::function<void(const sstring&)>;

using dhandler = seastar::httpd2::directory_handler;

class routes {
public:
    user_callback handle(const sstring &path);
    user_callback handle_push();
    routes& add(const method type, const sstring &path, user_callback handler);
    routes& add_on_push(const sstring &path, user_callback handler, user_callback push_handler);
    routes& add_on_client(client_callback handler);
    sstring& get_push_path() { return _push_path; }
    routes& add_directory_handler(dhandler *handler);
    ~routes() {
        delete _directory_handler;
    }
private:
    std::unordered_map<sstring, user_callback> _path_to_handler;
    user_callback _push_handler;
    sstring _push_path;
public:
    dhandler *_directory_handler {nullptr};
public:
    client_callback _client_handler;
};

class http2_stream {
public:
    http2_stream() = default;
    http2_stream(const int32_t id, routes *routes_);
    http2_stream(const int32_t id, lw_shared_ptr<request> req);
    int32_t get_id() const {
        return _id;
    }
    future<> eat_request(bool promised_stream = false);
    bool push() const;
    void update_request(request_feed &data);
    void commit_response(bool promised = false);
    void move_push_rep();
    response &get_response();
private:
    int32_t _id {0};
    lw_shared_ptr<request> _req;
    std::unique_ptr<response> _rep, _promised_rep;
    routes *_routes {nullptr};
};

enum class session_t {client, server};

namespace legacy = seastar::httpd;

template<session_t session_type = session_t::server>
class http2_connection final : public legacy::session {
public:
    explicit http2_connection(routes* routes_, connected_socket&& fd,
                     socket_address addr = socket_address());
    future<> process() override;
    void shutdown() override;
    output_stream<char>& out() override;

    ~http2_connection();
    void init();
    future<> process_internal(bool start_with_reading = true);
    int resume(const http2_stream &stream);
    int submit_response(http2_stream &stream);
    int submit_push_promise(http2_stream &stream);
    int submit_request(lw_shared_ptr<request> _request);
    void create_stream(const int32_t stream_id, lw_shared_ptr<request> req);
    void eat_server_rep(data_chunk_feed data);
    unsigned pending_streams() const {
        return _streams.size();
    }
private:
    nghttp2_session *_session {nullptr};
    bool _done {false};
    std::unordered_map<int32_t, std::unique_ptr<http2_stream>> _streams;
    connected_socket _fd;
    input_stream<char> _read_buf;
    output_stream<char> _write_buf;
    routes *_routes {nullptr};
    constexpr static auto _streams_limit = 100u;
    std::vector<lw_shared_ptr<request>> _remaining_reqs;
    bool _start_with_reading;

    enum class ops
    {
        on_begin_headers,
        on_frame_recv,
        on_header,
        on_data_chunk_recv,
        on_stream_close,
        on_frame_send,
        on_frame_not_send
    };

    future<> send_tx();
    int submit_request_nghttp2(lw_shared_ptr<request> _request);
    future<> internal_process();
    void dump_frame(nghttp2_frame_type frame_type, const char *direction = "---------------------------->");
    void receive_nghttp2(const uint8_t *data, size_t len);
    int send_nghttp2(const uint8_t **data);
    int handle_remaining_reqs();
    int consume_frame(nghttp2_internal_data &&data, ops state);
    void create_stream(const int32_t stream_id);
    void close_stream(const int32_t stream_id);
    http2_stream *find_stream(const int32_t stream_id);
};

}
}
