# Seastar HTTP/2 Prototype
## Overview
This repository contains proof of concept HTTP/2 implementation in Seastar based on Nghttp2 library.
Some key concepts/motivations:
* Usage of Nghttp2 as it's well known, stable and fast library. Implementation of HTTP/2 from the scratch would be much more time-consuming.
* Simple integration with existing HTTP/1.1 implementation in Seastar. To make this implementation as less intrusive as possible whole HTTP/2
  handling was put in http2_connection class. Consequently httpd listens on 2 ports - 10000 for HTTP/1.1 connections and 3000 for HTTP/2 connections.
* Both h2c (over TCP) and h2 (over TLS) are supported.    
## Dependencies
Libraries and frameworks:
* Seastar framework: http://seastar.io/
* Nghttp2 library: https://nghttp2.org/
Compilers:
* C++17 compiler is required
## Tweaking
* quick start:

./build/release/apps/httpd/httpd --node=server --debug=true --port=3000
curl --http2-prior-knowledge -G 127.0.0.1:3000

Dumps:

./build/release/apps/httpd/httpd --node=server --debug=true --port=3000
Seastar HTTP/1.1 legacy server listening on port 10000 ...
Seastar HTTP/2 server listening on port 3000 ...
method: GET
path: /
scheme: http

curl --http2-prior-knowledge -G 127.0.0.1:3000
handle /

* example of file transfer (with debug output enabled):

TODO

* debug verbose output of frames exchange:

new session: 127.0.0.1:54700
state=1
[INFO] C ----------------------------> S (SETTINGS)
state=0
[INFO] C ----------------------------> S (HEADERS)
state=2
state=2
state=2
state=2
state=2
state=2
state=1
[INFO] C ----------------------------> S (HEADERS)
state=5
[INFO] C <---------------------------- S (SETTINGS)
state=5
[INFO] C <---------------------------- S (SETTINGS)
state=5
[INFO] C <---------------------------- S (HEADERS)
state=5
[INFO] C <---------------------------- S (DATA)
state=4
state=1
[INFO] C ----------------------------> S (SETTINGS)
state=1
[INFO] C ----------------------------> S (GOAWAY)

* running performance tests (a'la seawreck)

./build/release/apps/httpd/httpd --node=server --tls=false --debug=false --port=3000
./build/release/apps/httpd/httpd --node=client --tls=false --con=500 --req=4000

## Performance and scalability

* performance tests executed only per one shard on one machine and comparision with old implementation 

** HTTP/2 sserver

./build/release/apps/httpd/httpd --node=server --tls=false --debug=false --port=3000 -c 1
./build/release/apps/httpd/httpd --node=client --tls=false --con=500 --req=4000 -c 1
WARN  2018-07-21 10:50:01,324 [shard 0] seastar - Unable to set SCHED_FIFO scheduling policy for timer thread; latency impact possible. Try adding CAP_SYS_NICE
established tcp connections
Total responses: 2000000
Req/s: 224330
Avg resp time: 4.45772 us

** legacy HTTP/1.1 server

./build/release/apps/httpd/httpd --node=server --tls=false --debug=false --port=3000 -c 1
./build/release/apps/seawreck/seawreck --smp 1 --server 127.0.0.1:3000
WARN  2018-06-09 14:15:33,693 [shard 0] seastar - Unable to set SCHED_FIFO scheduling policy for timer thread; latency impact possible. Try adding CAP_SYS_NICE
========== http_client ============
Server: 127.0.0.1:3000
Connections: 100
Requests/connection: dynamic (timer based)
Requests on cpu  0: 624597
Total cpus: 1
Total requests: 624597
Total time: 10.003952
Requests/sec: 62435.024867
==========     done     ============

* performance tests executed with all shard on 2 hosts

TODO
