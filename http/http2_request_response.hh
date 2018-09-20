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
 * Copyright (C) 2017 ScyllaDB Ltd.
 */
#pragma once

#include "core/sstring.hh"
#include <nghttp2/nghttp2.h>
#include <optional>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <array>
#include <vector>
#include <variant>
#include <stdexcept>
#include <initializer_list>

namespace seastar {
namespace httpd2 {

constexpr inline auto debug_on = false;
constexpr inline auto debug_on_file = false;

class nghttp2_exception final : public std::exception {
public:
    nghttp2_exception(const char *msg, int err)
        : _msg(msg) {
        _msg += sstring(": ") + nghttp2_strerror(err);
    }
    const char* what() const noexcept override {
        return _msg.c_str();
    }
private:
    sstring _msg;
};

enum class method
{
    GET
};

using request_feed = std::tuple<const uint8_t*, size_t, const uint8_t*, size_t>;
using data_chunk_feed = std::tuple<const uint8_t*, size_t>;
using nghttp2_internal_data = std::variant<const nghttp2_frame*, int32_t, data_chunk_feed,
                                            std::tuple<const nghttp2_frame*, request_feed>>;

class headers_utils {
    static uint8_t* do_cast(const char *ptr) {
        return const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(ptr));
    }
protected:
    std::vector<std::pair<sstring, sstring>> _headers;
    std::vector<nghttp2_nv> _nva;

    template <size_t size>
    static nghttp2_nv make_header(const char (&name)[size], const sstring &value) {
        return {(uint8_t *)name, do_cast(value.data()), size - 1, value.size(),
                    NGHTTP2_NV_FLAG_NO_COPY_NAME};
    }

    static nghttp2_nv make_header(const sstring &name, const sstring &value) {
        return {do_cast(name.data()), do_cast(value.data()), name.size() , value.size(),
                    NGHTTP2_NV_FLAG_NO_COPY_NAME};
    }
public:
    headers_utils() = default;
    headers_utils(std::vector<std::pair<sstring, sstring> > && headers)
        : _headers(headers) {}

    void done() {
        for (const auto &item : _headers) {
            _nva.push_back(make_header(item.first, item.second));
        }
    }
    size_t size() const {
        return _nva.size();
    }
    const nghttp2_nv *data() const {
        return _nva.data();
    }
    void clear() {
        _nva.clear();
    }
};

class request : public headers_utils {
public:
    request() = default;
    request(std::initializer_list<std::pair<sstring, sstring> > headers);
    void add_header(const request_feed &feed);
    request *add_header(const sstring& header, const sstring& value);
    // minimal set of headers according RFC
    sstring _method;
    sstring _scheme;
    sstring _path;
};

class response : public headers_utils {
public:
    uint32_t _status_code {200u};
    nghttp2_data_provider _prd;
    sstring _body;

    void add_headers(std::initializer_list<std::pair<sstring, sstring> > headers);
    response *add_header(const sstring& header, const sstring& value);
    void flush_body() {
        _body_head = _body.c_str();
        _prd.source.ptr = reinterpret_cast<void*>(this);
        _prd.read_callback = [](auto, auto, auto buf, auto length, auto flags, auto source, auto) -> ssize_t {
            auto rep = reinterpret_cast<response*>(source->ptr);
            return rep->flush_body(buf, length, flags);
        };
    }

    void set_status(uint32_t code) {
        _status_code = code;
    }

private:
    const char *_body_head {nullptr};

    size_t flush_body(uint8_t *out_buffer, size_t, uint32_t *out_flags);
};

}
}
