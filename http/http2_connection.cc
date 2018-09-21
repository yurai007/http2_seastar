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

#include "http2_connection.hh"

namespace seastar {
namespace httpd2 {

http2_stream::http2_stream(const int32_t id, routes *routes_)
    : _id(id), _req(make_lw_shared<request>()), _routes(routes_)
{}

http2_stream::http2_stream(const int32_t id, lw_shared_ptr<request> req)
    : _id(id), _req(req) {
    assert(_req);
}

future<> http2_stream::eat_request(bool promised_stream) {
    auto user_handler = (!promised_stream)? _routes->handle(_req->_path) : _routes->handle_push();
    if (!user_handler) {
        _rep = std::make_unique<response>();
        auto user_file_handler = _routes->_directory_handler;
        assert(user_file_handler);
        return user_file_handler->handle(_req, std::move(_rep))
                .then([this](auto rep){
            _rep = std::move(rep);
            assert(_rep);
            return make_ready_future<>();
        });
    } else {
        _rep = std::make_unique<response>();
        return user_handler(std::move(_req), std::move(_rep))
                .then([this](auto req_rep){
            std::tie(_req, _rep) = std::move(req_rep);
            assert(_rep);
            return make_ready_future<>();
        });
    }
}

bool http2_stream::push() const {
    assert(_req);
    return (_req->_path == _routes->get_push_path());
}

void http2_stream::update_request(request_feed &data) {
    _req->add_header(data);
}

void http2_stream::commit_response(bool promised) {
    assert(_rep);
    if (!promised) {
        _rep->flush_body();
        _rep->clear();
        sstring status = (_rep->_status_code == 200)? "200" : to_sstring(_rep->_status_code);
        sstring length = to_sstring(static_cast<int>(_rep->_body.size()));
        _rep->add_headers({{":status", status},
                           {"date", *_routes->_date},
                           {"content-length", length}});
    } else {
        _rep->clear();
    }
    _rep->done();
}

void http2_stream::move_push_rep() {
    _promised_rep = std::move(_rep);
    _rep = std::make_unique<response>();
}

response &http2_stream::get_response() {
    return *_rep;
}

template<session_t session_type>
http2_connection<session_type>::http2_connection(routes *routes_, connected_socket&& fd, socket_address addr)
    : _fd(std::move(fd)), _read_buf(_fd.input()), _write_buf(_fd.output()), _routes(routes_) {
    assert(_routes);
    if constexpr (session_type == session_t::client) {
        assert(_routes->_client_handler);
    }
    if (debug_on) {
        fmt::print("new session: {}\n", addr);
    }
    nghttp2_session_callbacks *callbacks;
    static auto get_impl = [](void *ptr) { return reinterpret_cast<http2_connection*>(ptr); };
    int rv = nghttp2_session_callbacks_new(&callbacks);
    if (rv != 0) {
        throw nghttp2_exception("nghttp2_session_callbacks_new", rv);
    }
    if constexpr (session_type == session_t::client) {
        nghttp2_session_callbacks_set_on_frame_send_callback(callbacks,
            [](nghttp2_session *, const nghttp2_frame *frame, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_frame_send>(frame);
            });

        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
            [](nghttp2_session*, const nghttp2_frame *frame, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_frame_recv>(frame);
            });

        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks,
            [](nghttp2_session*, int32_t stream_id, uint32_t, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_stream_close>(stream_id);
            });

        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
            [](nghttp2_session*, uint8_t, int32_t, const uint8_t *data, size_t len, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_data_chunk_recv>(std::make_tuple(data, len));
            });

        nghttp2_session_callbacks_set_error_callback2(callbacks,
            [](nghttp2_session *, int lib_error_code, const char *msg, size_t, void *){
                fmt::print("error: {} {}\n", lib_error_code, msg);
                return static_cast<int>(NGHTTP2_ERR_CALLBACK_FAILURE);
            });
        rv = nghttp2_session_client_new(&_session, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        if (rv != 0 || !_session) {
            throw nghttp2_exception("nghttp2_session_client_new", rv);
        }
        rv = nghttp2_submit_settings(_session, NGHTTP2_FLAG_NONE, nullptr, 0);
        if (rv != 0) {
            throw nghttp2_exception("nghttp2_submit_settings", rv);
        }
    } else {
        nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks,
            [](nghttp2_session*, const nghttp2_frame *frame, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_begin_headers>(frame);
            });

        nghttp2_session_callbacks_set_on_header_callback(callbacks,
            [](nghttp2_session*, const nghttp2_frame *frame, const uint8_t *name, size_t namelen,
                            const uint8_t *value, size_t valuelen, uint8_t, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_header>(std::make_tuple(frame,
                                    std::make_tuple(name, namelen, value, valuelen)));
            });

        nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks,
            [](nghttp2_session*, const nghttp2_frame *frame, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_frame_recv>(frame);
            });

        nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
            [](nghttp2_session*, uint8_t, int32_t, const uint8_t*, size_t, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_data_chunk_recv>({});
            });

        nghttp2_session_callbacks_set_on_stream_close_callback(callbacks,
            [](nghttp2_session*, int32_t stream_id, uint32_t, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_stream_close>(stream_id);
            });

        nghttp2_session_callbacks_set_on_frame_send_callback(callbacks,
            [](nghttp2_session *, const nghttp2_frame *frame, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_frame_send>(frame);
            });

        nghttp2_session_callbacks_set_on_frame_not_send_callback(callbacks,
            [](nghttp2_session *, const nghttp2_frame *, int, void *user_data) {
                http2_connection* con = get_impl(user_data);
                return con->consume_frame<ops::on_frame_not_send>({});
            });

        nghttp2_session_callbacks_set_error_callback2(callbacks,
            [](nghttp2_session *, int lib_error_code, const char *msg, size_t, void *){
                fmt::print("error: {} {}\n", lib_error_code, msg);
                return static_cast<int>(NGHTTP2_ERR_CALLBACK_FAILURE);
            });
        rv = nghttp2_session_server_new(&_session, callbacks, this);
        nghttp2_session_callbacks_del(callbacks);
        if (rv != 0 || !_session) {
            throw nghttp2_exception("nghttp2_session_server_new", rv);
        }
        nghttp2_settings_entry entry{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, _streams_limit};
        rv = nghttp2_submit_settings(_session, NGHTTP2_FLAG_NONE, &entry, 1);
        if (rv != 0) {
            throw nghttp2_exception("nghttp2_submit_settings", rv);
        }
    }
}

template<session_t session_type>
void http2_connection<session_type>::eat_server_rep(data_chunk_feed data) {
    const auto *ptr = reinterpret_cast<const char*>(std::get<0>(data));
    const sstring response(ptr, std::get<1>(data));
    _routes->_client_handler(response);
}

void dump_buffer(temporary_buffer<char> buffer, const char *direction) {
    if (!debug_on)
        return;
    fmt::print("{}:     {}B\n", direction, buffer.size());
    fmt::print("content:    ");
    for (auto i = 0u; i < buffer.size(); i++)
        fmt::print(" {0:x}", int((unsigned char)(buffer[i])));
    fmt::print("\n");
}

void dump_frame_type(nghttp2_frame_type frame_type,
                     const char *direction = "---------------------------->") {
    if (!debug_on)
        return;
    switch(frame_type) {
    case NGHTTP2_RST_STREAM:
        fmt::print("[INFO] C {} S (RST_STREAM)\n", direction);
        break;
    case NGHTTP2_SETTINGS:
        fmt::print("[INFO] C {} S (SETTINGS)\n", direction);
        break;
    case NGHTTP2_HEADERS:
        fmt::print("[INFO] C {} S (HEADERS)\n", direction);
        break;
    case NGHTTP2_WINDOW_UPDATE:
        fmt::print("[INFO] C {} S (WIN_UPDATE)\n", direction);
        break;
    case  NGHTTP2_DATA:
        fmt::print("[INFO] C {} S (DATA)\n", direction);
        break;
    case NGHTTP2_GOAWAY:
        fmt::print("[INFO] C {} S (GOAWAY)\n", direction);
        break;
    case NGHTTP2_PUSH_PROMISE:
        fmt::print("[INFO] C {} S (PUSH_PROMISE)\n", direction);
        break;
    default:
        break;
    }
}

template<session_t session_type>
future<> http2_connection<session_type>::process_internal(bool start_with_reading) {
    _start_with_reading = start_with_reading;
    return do_until([this] {return _done;}, [this] {
        return (_start_with_reading? _read_buf.read() : make_ready_future<temporary_buffer<char>>())
            .then([this] (temporary_buffer<char> buf) {

            if (_start_with_reading) {
                {
                    temporary_buffer<char> dump(buf.get(), buf.size());
                    dump_buffer(std::move(dump), "RX");
                }
                const uint8_t *data = (const uint8_t *)(buf.get());
                receive_nghttp2(data, buf.size());
                if (buf.size() == 0)
                {
                    _done = true;
                    return make_ready_future<>();
                }
            } else
               _start_with_reading = true;

            return process_send();
        });
    }).then_wrapped([this] (future<> f) {
        try {
            f.get();
        } catch (std::exception& ex) {
            std::cerr << "process_internal failed: " << ex.what() << std::endl;
        }
        return _write_buf.close();
    }).finally([this] {
        return _read_buf.close();
    });
}

template<session_t session_type>
future<> http2_connection<session_type>::process() {
    return process_internal();
}

template<session_t session_type>
future<> http2_connection<session_type>::process_send() {
    auto end = make_lw_shared<bool>(false);
    return do_until([this, end] {return *end;}, [this, end] {
        const uint8_t *data = nullptr;
        auto bytes = send_nghttp2(&data);
        if (bytes == 0) {
            *end = true;
            return make_ready_future<>();
        } else {
            {
                temporary_buffer<char> dump(reinterpret_cast<const char*>(data), bytes);
                dump_buffer(std::move(dump), "TX");
            }
            return _write_buf.write(reinterpret_cast<const char*>(data), bytes);
        }
    })
    .then([this](){
        return _write_buf.flush();
    });
}

template<session_t session_type>
int http2_connection<session_type>::resume(const http2_stream &stream) {
    return nghttp2_session_resume_data(_session, stream.get_id());
}

template<session_t session_type>
void http2_connection<session_type>::shutdown() {
    _fd.shutdown_input();
    _fd.shutdown_output();
}

template<session_t session_type>
output_stream<char>& http2_connection<session_type>::out() {
    return _write_buf;
}

template<session_t session_type>
int http2_connection<session_type>::submit_response(http2_stream &stream) {
    auto &response = stream.get_response();
    return nghttp2_submit_response(_session, stream.get_id(), response.data(),
                                   response.size(), &response._prd);
}

template<session_t session_type>
int http2_connection<session_type>::submit_push_promise(http2_stream &stream) {
    auto &response = stream.get_response();
    return nghttp2_submit_push_promise(_session, NGHTTP2_FLAG_NONE, stream.get_id(), response.data(),
                                       response.size(), nullptr);
}

template<session_t session_type>
int http2_connection<session_type>::submit_request_nghttp2(lw_shared_ptr<request> request_) {
    return nghttp2_submit_request(_session, nullptr, request_->data(), request_->size(),
                                  nullptr, nullptr);
}

template<session_t session_type>
void http2_connection<session_type>::reset_stream(int32_t stream_id, uint32_t error_code) {
    nghttp2_submit_rst_stream(_session, NGHTTP2_FLAG_NONE, stream_id, error_code);
}

template<session_t session_type>
int http2_connection<session_type>::submit_request(lw_shared_ptr<request> request_) {
    if (pending_streams() < _streams_limit) {
        auto stream_id = submit_request_nghttp2(request_);
        if (stream_id >= 0) {
            create_stream(stream_id, request_);
        }
        return stream_id;
    } else {
        _remaining_reqs.emplace_back(request_);
        return 0;
    }
}

template<session_t session_type>
int http2_connection<session_type>::handle_remaining_reqs() {
    if (!_remaining_reqs.empty() && (pending_streams()-1) < _streams_limit) {
        auto rc = submit_request(_remaining_reqs.back());
        _remaining_reqs.pop_back();
        return rc;
    }
    return 1;
}

template<session_t session_type>
void http2_connection<session_type>::dump_frame(nghttp2_frame_type frame_type, const char *direction) {
    if constexpr (session_type == session_t::client) {
         if (strcmp(direction, "---------------------------->") == 0)
             dump_frame_type(frame_type, "<----------------------------");
         else
             dump_frame_type(frame_type, "---------------------------->");
    }
    else
        dump_frame_type(frame_type, direction);
}

template<session_t session_type>
void http2_connection<session_type>::receive_nghttp2(const uint8_t *data, size_t len) {
    auto rv = nghttp2_session_mem_recv(_session, data, len);
    if (rv < 0) {
        throw nghttp2_exception("nghttp2_session_mem_recv", rv);
    }
}

template<session_t session_type>
int http2_connection<session_type>::send_nghttp2(const uint8_t **data) {
    auto rv = nghttp2_session_mem_send(_session, data);
    if (rv < 0) {
        throw nghttp2_exception("nghttp2_session_mem_send", rv);
    }
    return rv;
}

static int error() {
    return NGHTTP2_ERR_CALLBACK_FAILURE;
}

template<session_t session_type>
template<ops state>
int http2_connection<session_type>::consume_frame(nghttp2_internal_data && data) {
    if (debug_on) {
        fmt::print("state={}\n", static_cast<int>(state));
    }
    if constexpr (state == ops::on_frame_send) {
        auto frame = std::get<const nghttp2_frame*>(data);
        auto type = static_cast<nghttp2_frame_type>(frame->hd.type);
        dump_frame(type, "<----------------------------");

        if (type != NGHTTP2_PUSH_PROMISE) {
            if constexpr (session_type == session_t::client) {
                if ((type == NGHTTP2_GOAWAY)) {
                    _done = true;
                }
            }
          return 0;
        }
        else
        {
            // we can push response on stream 2 only after succesful push promise send
            auto id = frame->push_promise.promised_stream_id;
            auto promised_stream = find_stream(id);
            if (!promised_stream)
                return 0;

            promised_stream->eat_request(true).then([promised_stream, this](){
                promised_stream->commit_response();
                auto rc = submit_response(*promised_stream);
                if (rc != 0) {
                    reset_stream(promised_stream->get_id(), NGHTTP2_INTERNAL_ERROR);
                }
                return process_send();
            });
        }
    } else if constexpr (state == ops::on_begin_headers) {
        auto frame = std::get<const nghttp2_frame*>(data);
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
            return 0;
        }

        create_stream(frame->hd.stream_id);
        dump_frame(static_cast<nghttp2_frame_type>(frame->hd.type));
    } else if constexpr (state == ops::on_header) {
        // request creation
        auto [frame, feed] = std::get<std::tuple<const nghttp2_frame*, request_feed>>(data);
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
            return 0;
        }
        auto stream = find_stream(frame->hd.stream_id);
        if (!stream)
            return 0;
        stream->update_request(feed);
    } else if constexpr (state == ops::on_data_chunk_recv) {
        if constexpr (session_type == session_t::client) {
            eat_server_rep(std::get<data_chunk_feed>(data));
        }
        return 0;
    } else if constexpr (state == ops::on_frame_recv) {
        // handling request + commiting response
        auto frame = std::get<const nghttp2_frame*>(data);
        auto stream = find_stream(frame->hd.stream_id);
        auto type = static_cast<nghttp2_frame_type>(frame->hd.type);
        dump_frame(type);

        switch (type) {
        case NGHTTP2_DATA: {
            if constexpr (session_type == session_t::client)
                break;
            if (!stream)
                break;
            if (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) {
                return error();
            }
            break;
        }
        case NGHTTP2_HEADERS: {
            if (!stream || frame->headers.cat != NGHTTP2_HCAT_REQUEST) {
                break;
            }
            // now normal flow for stream 1 - commit response
            stream->eat_request().then([stream, this](){
                if (stream->push()) {
                    // 1. stream 1 has req and some rep was deliverd by callback
                    // 2. simulate push reponse and get stream 2
                    stream->commit_response(true);
                    auto id = submit_push_promise(*stream);
                    if (id < 0) {
                        throw nghttp2_exception("submit_push_promise", id);
                    }
                    stream->move_push_rep();
                    create_stream(id);
                    auto promised_strm = find_stream(id);
                    assert(promised_strm != nullptr);
                }
                stream->commit_response();
                auto rc = submit_response(*stream);
                if (rc != 0) {
                    reset_stream(stream->get_id(), NGHTTP2_INTERNAL_ERROR);
                }
                rc = resume(*stream);
                if (!rc)
                    throw nghttp2_exception("resume", rc);
                return process_send();
            });
            break;
        }
        default:
            break;
        }
    } else if constexpr (state == ops::on_frame_not_send) {
    } else if constexpr (state == ops::on_stream_close) {
        auto stream_id = std::get<int32_t>(data);
        auto stream = find_stream(stream_id);
        if (!stream)
            return 0;
        close_stream(stream_id);
        if constexpr (session_type == session_t::client) {
            if (pending_streams() > 0) {
                auto rc = handle_remaining_reqs();
                if (rc <= 0) {
                    return error();
                }
            }
            if (_remaining_reqs.empty() && (pending_streams() == 0)) {
                auto rc = nghttp2_session_terminate_session(_session, NGHTTP2_NO_ERROR);
                if (rc != 0) {
                    return error();
                }
            }
        }
    } else {
        return error();
    }
    return 0;
}

template<session_t session_type>
void http2_connection<session_type>::create_stream(const int32_t stream_id, lw_shared_ptr<request> req) {
    _streams.emplace(stream_id, std::make_unique<http2_stream>(stream_id, req));
}

template<session_t session_type>
http2_connection<session_type>::~http2_connection() {
    nghttp2_session_del(_session);
}

template<session_t session_type>
void http2_connection<session_type>::create_stream(const int32_t stream_id) {
    _streams.emplace(stream_id, std::make_unique<http2_stream>(stream_id, _routes));
}

template<session_t session_type>
void http2_connection<session_type>::close_stream(const int32_t stream_id) {
    _streams.erase(stream_id);
}

template<session_t session_type>
http2_stream *http2_connection<session_type>::find_stream(const int32_t stream_id) {
    auto it = _streams.find(stream_id);
    if (it == _streams.end())
        return nullptr;
    return (*it).second.get();
}

user_callback routes::handle(const sstring &path) {
    return _path_to_handler[path];
}

user_callback routes::handle_push() {
     return _push_handler;
}

routes& routes::add(const method type, const sstring &path, user_callback handler) {
    _path_to_handler[path] = handler;
    return *this;
}

routes &routes::add_on_push(const sstring &path, user_callback handler, user_callback push_handler) {
    _push_path = path;
    _path_to_handler[_push_path] = handler;
    _push_handler = push_handler;
    return *this;
}

routes &routes::add_on_client(client_callback handler) {
    _client_handler = handler;
    return *this;
}

routes &routes::add_directory_handler(dhandler *handler) {
    _directory_handler = handler;
    return *this;
}

template class http2_connection<session_t::server>;
template class http2_connection<session_t::client>;

}
}
