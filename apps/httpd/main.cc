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
 * Copyright 2015 Cloudius Systems
 */

#include "http/httpd.hh"
#include "http/handlers.hh"
#include "http/function_handlers.hh"
#include "http/file_handler.hh"
#include "apps/httpd/demo.json.hh"
#include "http/api_docs.hh"
#include "http/http2_connection.hh"
#include "http/http2_client.hh"

namespace bpo = boost::program_options;
namespace h2 = seastar::httpd2;

using namespace seastar;
using namespace httpd;

static auto debug_handlers = false;
constexpr static auto hardcoded_path = "/home/yurai/seastar/http2_reload/test_http2/";

class handl : public httpd::handler_base {
public:
    virtual future<std::unique_ptr<reply> > handle(const sstring& path,
            std::unique_ptr<request> req, std::unique_ptr<reply> rep) {
        rep->_content = "hello";
        rep->done("html");
        return make_ready_future<std::unique_ptr<reply>>(std::move(rep));
    }
};

void set_routes(routes& r, h2::routes &rhttp2) {
    function_handler* h1 = new function_handler([](const_req req) {
         if (debug_handlers) {
             fmt::print("HTTP/1.1 method: {}\npath: {}\n", req._method, req._url);
         }
        return "hello";
    });
    function_handler* h2 = new function_handler([](std::unique_ptr<request> req) {
        return make_ready_future<json::json_return_type>("json-future");
    });
    r.add(operation_type::GET, url("/"), h1);
    r.add(operation_type::GET, url("/jf"), h2);
    r.add(operation_type::GET, url("/file").remainder("path"),
            new directory_handler("/"));
    demo_json::hello_world.set(r, [] (const_req req) {
        demo_json::my_object obj;
        obj.var1 = req.param.at("var1");
        obj.var2 = req.param.at("var2");
        demo_json::ns_hello_world::query_enum v = demo_json::ns_hello_world::str2query_enum(req.query_parameters.at("query_enum"));
        // This demonstrate enum conversion
        obj.enum_var = v;
        return obj;
    });

    rhttp2.add(h2::method::GET, "/", [](auto req, auto rep){
        if (debug_handlers) {
            fmt::print("method: {}\npath: {}\nscheme: {}\n", req->_method, req->_path, req->_scheme);
        }
        rep->_body = "handle /\n";
        return make_ready_future<std::tuple<lw_shared_ptr<h2::request>, std::unique_ptr<h2::response>>>(
                                    std::make_tuple(std::move(req), std::move(rep)));
    }).add(h2::method::GET, "/get", [](auto req, auto rep){
        if (debug_handlers) {
            fmt::print("method: {}\npath: {}\nscheme: {}\n", req->_method, req->_path, req->_scheme);
        }
        rep->_body = "hello!";
        return make_ready_future<std::tuple<lw_shared_ptr<h2::request>, std::unique_ptr<h2::response>>>(
                                    std::make_tuple(std::move(req), std::move(rep)));
    })
    .add_directory_handler(new seastar::httpd2::directory_handler(hardcoded_path))
    .add_on_push("/push",
    [](auto req, auto rep){
        rep->add_headers({{":method", "GET"}, {":scheme", "http"},
           {":authority", "localhost:3000"}, {":path", "/push/1"}});
        rep->_body = "GET REP BODY\n";
        if (debug_handlers) {
            fmt::print("push 1\n");
        }
        return make_ready_future<std::tuple<lw_shared_ptr<h2::request>, std::unique_ptr<h2::response>>>(
                                     std::make_tuple(std::move(req), std::move(rep)));
    },
    [](auto req, auto rep) {
        assert(req);
        assert(rep);
        rep->_body = "PUSH REP BODYPUSH REP BODYPUSH REP BODYPUSH REP BODYPUSH REP BODYPUSH REP BODYPUSH REP BODY\n";
        if (debug_handlers) {
            fmt::print("push 2\n");
        }
        return make_ready_future<std::tuple<lw_shared_ptr<h2::request>, std::unique_ptr<h2::response>>>(
                                     std::make_tuple(std::move(req), std::move(rep)));
    });
}

void set_handler(h2::http_client& client) {
    client._routes.add_on_client([&](const auto rep){
        client._responses++;
        if (debug_handlers) {
            fmt::print("recieved {}B: {}\n", rep.size(), rep);
        }
    });
}

future<> server(bpo::variables_map config) {
    uint16_t port = config["port"].as<uint16_t>();
    auto with_tls = config["tls"].as<bool>();
    auto server = make_lw_shared<http_server_control>();
    auto rb = make_shared<api_registry_builder>("apps/httpd/");
    return server->start().then([server] {
        return server->set_routes(set_routes);
    }).then([server, rb]{
        return server->set_routes([rb](routes& r){rb->set_api_doc(r);});
    }).then([server, rb]{
        return server->set_routes([rb](routes& r) {rb->register_function(r, "demo", "hello world application");});
    }).then([server, port, with_tls] {
        return server->listen(port, with_tls);
    }).then([server, port] {
        fmt::print("Seastar HTTP/1.1 legacy server listening on port 10000 ...\n");
        fmt::print("Seastar HTTP/2 server listening on port {} ...\n", port);
        engine().at_exit([server] {
            return server->stop();
        });
    });
}

future<> http2_client(bpo::variables_map config) {
    const auto with_tls = config["tls"].as<bool>();
    const auto connections = config["con"].as<uint16_t>();
    const auto reqs = config["req"].as<uint16_t>();
    auto client = new distributed<h2::http_client>;
    auto req = new h2::request({ {":method", "GET"}, {":path", "/get"}, {":scheme", "https"},
        {":authority", "127.0.0.1:3000"}, {"accept", "*/*"}, {"user-agent", "nghttp2/" NGHTTP2_VERSION} });

    const auto started = steady_clock_type::now();
    return client->start()
            .then([client, with_tls, connections](){
                ipv4_addr server_addr = {"127.0.0.1", 3000};
                return client->invoke_on_all(&h2::http_client::connect, connections, server_addr, with_tls);
            })
            .then([client, req](){
                return client->invoke_on_all([](h2::http_client& client) {
                    set_handler(client);
                });
            })
            .then([client, req, reqs](){
                return client->invoke_on_all(&h2::http_client::run, req, reqs);
            })
            .then([client](){
                return client->map_reduce(adder<uint64_t>(), &h2::http_client::responses);
            })
            .then([client, req, started](auto total_responses){
                const auto finished = steady_clock_type::now();
                const auto responses = static_cast<double>(total_responses);
                auto elapsed = finished - started;
                auto secs = static_cast<double>(elapsed.count() / 1000000000.0);
                fmt::print("Total responses: {}\nReq/s: {}\nAvg resp time: {} us\n", total_responses, responses / secs, (secs / responses) * 1000000.0);
                return client->stop().then([client, req] {
                    delete req;
                    delete client;
                    return make_ready_future<>();
                });
            });
}

int main(int ac, char** av) {
    app_template app;
    app.add_options()("node,n", bpo::value<std::string>()->default_value("server"), "Node");
    app.add_options()("port", bpo::value<uint16_t>()->default_value(3000), "HTTP/2 port");
    app.add_options()("tls,t", bpo::value<bool>()->default_value(false), "TLS enabled");
    app.add_options()("con", bpo::value<uint16_t>()->default_value(500u), "Connections number");
    app.add_options()("req,r", bpo::value<uint16_t>()->default_value(4000u), "Requests number per client connection");
    app.add_options()("debug,d", bpo::value<bool>()->default_value(false), "Debugging info from handlers");

    return app.run_deprecated(ac, av, [&] {
        auto&& config = app.configuration();
        auto&& node = config["node"].as<std::string>();
        debug_handlers = config["debug"].as<bool>();
        if (node == "server") {
            return server(config);
        } else {
            return http2_client(config);
        }
    });
}
