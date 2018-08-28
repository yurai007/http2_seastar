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

#include "http2_request_response.hh"
#include <fmt/ostream.h>
#include <fmt/printf.h>

namespace seastar {
namespace httpd2 {

request::request(std::initializer_list<std::pair<sstring, sstring> > headers)
    : headers_utils(headers)  {}

void request::add_header(const request_feed &feed) {
    const sstring header(reinterpret_cast<const char*>(std::get<0>(feed)));
    const sstring content(reinterpret_cast<const char*>(std::get<2>(feed)));
    if (header == ":method") {
        _method = std::move(content);
    } else if (header == ":path") {
         _path = std::move(content);
    } else if (header == ":scheme") {
         _scheme = std::move(content);
    }
}

request* request::add_header(const sstring& header, const sstring& value) {
    _headers.push_back({header,value});
    return this;
}

void response::add_headers(std::initializer_list<std::pair<sstring, sstring> > headers) {
    _headers.insert(_headers.end(), headers.begin(), headers.end());
}

response *response::add_header(const sstring &header, const sstring &value) {
    _headers.push_back({header,value});
    return this;
}

size_t response::flush_body(uint8_t *out_buffer, size_t, uint32_t *out_flags) {
    constexpr auto NGHTTP2_MAX_PAYLOADLEN = 16384L;
    auto remaining_part = (_body.c_str() + _body.size()) - _body_head;
    auto chunk_size = std::min(remaining_part, NGHTTP2_MAX_PAYLOADLEN);
    if (debug_on) {
        fmt::print("remaining body: {} chunk size: {}\n", remaining_part, chunk_size);
    }
    std::copy_n(_body_head, chunk_size, out_buffer);
    _body_head += chunk_size;
    if (_body_head - _body.c_str() == static_cast<long>(_body.size())) {
        *out_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return chunk_size;
}

}
}
