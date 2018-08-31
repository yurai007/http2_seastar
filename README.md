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
### Quick start
```sh
./build/release/apps/httpd/httpd --node=server --debug=true --port=3000  
curl --http2-prior-knowledge -G 127.0.0.1:3000  
```
**Dumps:**
```sh
./build/release/apps/httpd/httpd --node=server --debug=true --port=3000  
Seastar HTTP/1.1 legacy server listening on port 10000 ...  
Seastar HTTP/2 server listening on port 3000 ...  
method: GET  
path: /  
scheme: http  
```
```sh
curl --http2-prior-knowledge -G 127.0.0.1:3000  
handle /  
```
### Example of file transfer (with debug output enabled)
```sh
curl --http2-prior-knowledge --output out_faces.png -G 127.0.0.1:3000/http2rulez.com/public/assets/images/faces.png  
  % Total    % Received % Xferd  Average Speed   Time    Time     Time  Current  
                                 Dload  Upload   Total   Spent    Left  Speed  
100  387k  100  387k    0     0  3652k      0 --:--:-- --:--:-- --:--:-- 3652k  
```
```sh
./build/debug/apps/httpd/httpd --node=server --debug=true --port=3000 -c 1  
WARNING: debug mode. Not for benchmarking or production  
WARN  2018-08-31 15:41:05,184 seastar - Seastar compiled with default allocator, heap profiler not supported  
WARN  2018-08-31 15:41:05,206 [shard 0] seastar - Unable to set SCHED_FIFO scheduling policy for timer thread; latency impact possible. Try adding CAP_SYS_NICE  
Seastar HTTP/1.1 legacy server listening on port 10000 ...  
Seastar HTTP/2 server listening on port 3000 ...  
opened /home/yurai/seastar/http2_reload/test_http2//http2rulez.com/public/assets/images/faces.png  
remaining body: 396431 chunk size: 16384  
remaining body: 380047 chunk size: 16384  
remaining body: 363663 chunk size: 16384  
remaining body: 347279 chunk size: 16384  
remaining body: 330895 chunk size: 16384  
remaining body: 314511 chunk size: 16384  
remaining body: 298127 chunk size: 16384  
remaining body: 281743 chunk size: 16384  
remaining body: 265359 chunk size: 16384  
remaining body: 248975 chunk size: 16384  
remaining body: 232591 chunk size: 16384  
remaining body: 216207 chunk size: 16384  
remaining body: 199823 chunk size: 16384  
remaining body: 183439 chunk size: 16384  
remaining body: 167055 chunk size: 16384  
remaining body: 150671 chunk size: 16384  
remaining body: 134287 chunk size: 16384  
remaining body: 117903 chunk size: 16384  
remaining body: 101519 chunk size: 16384  
remaining body: 85135 chunk size: 16384  
remaining body: 68751 chunk size: 16384  
remaining body: 52367 chunk size: 16384  
remaining body: 35983 chunk size: 16384  
remaining body: 19599 chunk size: 16384  
remaining body: 3215 chunk size: 3215  
```
### Debug verbose output of frames exchange
```sh
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
```
### Running performance tests (a'la seawreck)
```sh
./build/release/apps/httpd/httpd --node=server --tls=false --debug=false --port=3000  
./build/release/apps/httpd/httpd --node=client --tls=false --con=500 --req=4000    
```
## Performance and scalability

### Performance tests executed only per one shard on one machine and comparision with old implementation 

**HTTP/2 server**
```sh
./build/release/apps/httpd/httpd --node=server --tls=false --debug=false --port=3000 -c 1  
./build/release/apps/httpd/httpd --node=client --tls=false --con=500 --req=4000 -c 1  
WARN  2018-07-21 10:50:01,324 [shard 0] seastar - Unable to set SCHED_FIFO scheduling policy for timer thread; latency impact   possible. Try adding CAP_SYS_NICE  
established tcp connections  
Total responses: 2000000  
Req/s: **224330**  
Avg resp time: 4.45772 us  
```
**Legacy HTTP/1.1 server**  
```sh
./build/release/apps/httpd/httpd --node=server --tls=false --debug=false --port=3000 -c 1  
./build/release/apps/seawreck/seawreck --smp 1 --server 127.0.0.1:3000  
WARN  2018-06-09 14:15:33,693 [shard 0] seastar - Unable to set SCHED_FIFO scheduling policy for timer thread; latency impact   possible. Try adding CAP_SYS_NICE  
========== http_client ============  
Server: 127.0.0.1:3000  
Connections: 100  
Requests/connection: dynamic (timer based)  
Requests on cpu  0: 624597  
Total cpus: 1  
Total requests: 624597  
Total time: 10.003952  
Requests/sec: **62435.024867**  
==========     done     ============  
```
Results are very promising - in this syntactic benchmark HTTP/2 server achieve more then 3x throughput then legacy HTTP/1.1 implementation. More details will be provided soon.
### Performance tests executed with all shard on 2 hosts

TODO
