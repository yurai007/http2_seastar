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
/*
 * Copyright (C) 2018 ScyllaDB Ltd.
 */

#pragma once

#include "http/http2_request_response.hh"
#include "http/http2_connection.hh"
#include "core/sstring.hh"
#include <cassert>

namespace seastar {

namespace h2 = seastar::httpd2;

enum frame {MAGIC_CHUNK = -1, SETTINGS = NGHTTP2_SETTINGS, WIN_UPDATE = NGHTTP2_WINDOW_UPDATE,
            HEADERS = NGHTTP2_HEADERS, DATA = NGHTTP2_DATA, PUSH_PROMISE = NGHTTP2_PUSH_PROMISE,
            GOAWAY = NGHTTP2_GOAWAY};

class http2_test_env {
public:
    http2_test_env(const http2_test_env&) = delete;
    http2_test_env &operator=(const http2_test_env&) = delete;
    http2_test_env() {
        nghttp2_session_callbacks *callbacks;
        auto rv = nghttp2_session_callbacks_new(&callbacks);
        assert(rv == 0);
        nghttp2_session_callbacks_set_on_frame_send_callback(callbacks, [](auto, auto, auto) { return 0; });
        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
            [](auto, auto, auto, const uint8_t *data, size_t len, void *ud) {
                auto env = static_cast<http2_test_env*>(ud);
                env->response = sstring(reinterpret_cast<const char*>(data), len);
                std::cout << env->response << "\n";
                return 0;
            });
        rv = nghttp2_session_client_new(&session, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        assert (rv == 0 && session);
    }

    sstring prepare_http2_request(h2::request &req) {
        auto rv = nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0);
        assert(rv == 0);
        req.done();
        auto stream_id = nghttp2_submit_request(session, nullptr, req.data(), req.size(), nullptr, nullptr);
        assert(stream_id >= 0);
        auto size = 0u;
        sstring raw_frames;
        for (;;) {
            const uint8_t *data = nullptr;
            auto bytes = nghttp2_session_mem_send(session, &data);
            assert(bytes >= 0);
            sstring raw_frame {reinterpret_cast<const char*>(data), static_cast<size_t>(bytes)};
            if (bytes == 0) {
                break;
            } else {
                dump_buffer(raw_frame);
            }
            size += bytes;
            raw_frames += raw_frame;
        }
        assert(size == 81);
        return raw_frames;
    }

    bool read_http2(const temporary_buffer<char>& b) {
        auto data = reinterpret_cast<const uint8_t *>(b.get());
        auto rv = nghttp2_session_mem_recv(session, data, b.size());
        return (rv >= 0);
    }

    void set_expected_response_body(const sstring &body) {
        expected_rep_body = body;
    }

    void set_handler(auto &server, h2::user_callback callback_) {
        callback = callback_;
        server->_routes_http2.add(h2::method::GET, "/test", callback);
    }

    http2_test_env &expect_on_stream(int stream_id) {
        return *this;
    }

    http2_test_env &incoming_chunk(frame _frame) {
        return *this;
    }

    template<class ...Args>
    http2_test_env &incoming_frames(Args... args) {
        return *this;
    }

    template<class ...Args>
    http2_test_env &then_outgoing_frames(Args ...args) {
        return *this;
    }

    template<class ...Args>
    http2_test_env &then_outgoing_data_frames(unsigned, unsigned) {
        return *this;
    }

    template<class ...Args>
    http2_test_env &outgoing_frames(Args ...args) {
        return *this;
    }

    static void dump_buffer(const sstring &buffer) {
        std::cout <<  buffer.size() << " B:     ";
        for (auto i = 0u; i < buffer.size(); i++)
            std::cout << (unsigned char)(buffer[i]);
        std::cout << "\n";
    }

    void expect_get_request_frames_flow() {
        expect_on_stream(0)
            .incoming_chunk(MAGIC_CHUNK)
            .incoming_frames(SETTINGS, WIN_UPDATE)
            .expect_on_stream(1)
            .incoming_frames(HEADERS, HEADERS)
            .expect_on_stream(2)
            .outgoing_frames(SETTINGS)
            .then_outgoing_frames(HEADERS, DATA)
            .expect_on_stream(0)
            .then_outgoing_frames(GOAWAY);
    }

    void expect_streaming_for_get_request_frames_flow(unsigned body_chunks, unsigned with_size) {
        expect_on_stream(0)
            .incoming_chunk(MAGIC_CHUNK)
            .incoming_frames(SETTINGS, WIN_UPDATE)
            .expect_on_stream(1)
            .incoming_frames(HEADERS, HEADERS)
            .expect_on_stream(2)
            .outgoing_frames(SETTINGS)
            .then_outgoing_frames(HEADERS, DATA)
            .then_outgoing_data_frames(body_chunks, with_size)
            .expect_on_stream(0)
            .then_outgoing_frames(GOAWAY);
    }

    void expect_server_push_frames_flow() {
        expect_on_stream(0)
            .incoming_chunk(MAGIC_CHUNK)
            .incoming_frames(SETTINGS, WIN_UPDATE)
            .expect_on_stream(1)
            .incoming_frames(HEADERS, HEADERS)
            .then_outgoing_frames(PUSH_PROMISE)
            .expect_on_stream(2)
            .outgoing_frames(SETTINGS)
            .then_outgoing_frames(HEADERS, DATA)
            .expect_on_stream(1)
            .outgoing_frames(SETTINGS)
            .then_outgoing_frames(HEADERS, DATA)
            .expect_on_stream(0)
            .then_outgoing_frames(GOAWAY);
    }

    promise<> done;
    sstring response;
    sstring expected_rep_body;
    bool frames_fulfilled {false};
private:
    h2::user_callback callback;
    nghttp2_session *session;
};

}
