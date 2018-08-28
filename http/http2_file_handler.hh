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

#include <algorithm>
#include <iostream>
#include "core/reactor.hh"
#include "core/fstream.hh"
#include "core/shared_ptr.hh"
#include "core/app-template.hh"
#include "exception.hh"
#include "http2_request_response.hh"

namespace seastar {

namespace httpd2 {

class directory_handler {
public:
    explicit directory_handler(const sstring& doc_root)
            :  doc_root(doc_root) {
    }

    future<std::unique_ptr<response>> handle(lw_shared_ptr<request> req, std::unique_ptr<response> rep) {
        sstring full_path = doc_root + req->_path;
        auto h = this;
        return engine().file_type(full_path).then(
                [h, full_path, req = std::move(req), rep = std::move(rep)](auto val) mutable {
                    if (val) {
                        return h->read(full_path, std::move(req), std::move(rep));
                    }
                    rep->set_status(404u);
                    return make_ready_future<std::unique_ptr<response>>(std::move(rep));
                });
    }

    sstring get_extension(const sstring& file) {
        size_t last_slash_pos = file.find_last_of('/');
        size_t last_dot_pos = file.find_last_of('.');
        sstring extension;
        if (last_dot_pos != sstring::npos && last_dot_pos > last_slash_pos) {
            extension = file.substr(last_dot_pos + 1);
        }
        return extension;
    }

    output_stream<char> get_stream(lw_shared_ptr<request> req,
            const sstring& extension, output_stream<char>&& s) {
        return std::move(s);
    }


    struct reader {
    public:
        reader(file f, std::unique_ptr<response> rep)
                : is(make_file_input_stream(std::move(f)))
                , _rep(std::move(rep)) {
        }
        input_stream<char> is;
        std::unique_ptr<response> _rep;

        // for input_stream::consume():
        using unconsumed_remainder = std::experimental::optional<temporary_buffer<char>>;
        future<unconsumed_remainder> operator()(temporary_buffer<char> data) {
            if (data.empty()) {
                return make_ready_future<unconsumed_remainder>(std::move(data));
            } else {
                _rep->_body.append(data.get(), data.size());
                return make_ready_future<unconsumed_remainder>();
            }
        }
    };

    future<std::unique_ptr<response>> read(
            sstring file_name,
            lw_shared_ptr<request> req,
            std::unique_ptr<response> rep) {
        sstring extension = get_extension(file_name);
        // TO DO: not sure if rep movement is OK
        return open_file_dma(file_name, open_flags::ro).then(
                    [file_name, rep = std::move(rep), extension, this, req = std::move(req)](file f) mutable {
            if (true) {
                std::cout << "opened " << file_name << "\n";
            }
            std::shared_ptr<reader> r = std::make_shared<reader>(std::move(f), std::move(rep));
            return r->is.consume(*r).then([r, extension, this, req = std::move(req)]() {
                return make_ready_future<std::unique_ptr<response>>(std::move(r->_rep));
            });
        });
    }

private:
    sstring doc_root;

};

}

}
