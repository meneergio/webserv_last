# Webserv — Defense Reference

> Quick-access answer sheet for the peer evaluation.
> Two terminals are recommended: one for the server, one for `curl` / `siege`.
> All commands assume you are in the project root (the directory containing `Makefile` and `default.conf`).

---

## 0. Setup before the evaluator arrives

```bash
# 1. Clean compile from scratch — verifies "no relink" rule
make fclean && make

# 2. Install siege (Ubuntu/Debian)
sudo apt-get update && sudo apt-get install -y siege

# 3. (Optional) prepare CGI tester binary
chmod +x cgi_tester ubuntu_cgi_tester 2>/dev/null
cp ubuntu_cgi_tester www/cgi-bin/cgi_tester 2>/dev/null
chmod +x www/cgi-bin/cgi_tester

# 4. Make sure the upload directory exists
mkdir -p www/uploads

# 5. Run the server
./webserv default.conf
```

Expected output:
```
Listening on 127.0.0.1:8080
Listening on 127.0.0.1:9090
```

---

## 1. Code review and questions

### What is an HTTP server?

An HTTP server is a program that listens on a TCP port, accepts client connections, parses HTTP requests sent over those connections, and replies with HTTP responses. A request is a method + URI + version + headers (+ optional body). A response is a status line + headers + body. The server can serve files from disk, run scripts (CGI), accept uploads, redirect, etc. HTTP/1.1 adds keep-alive (one connection, multiple requests) and chunked transfer encoding.

### Which function did we use for I/O multiplexing?

- **Linux:** `epoll` (`epoll_create1`, `epoll_ctl`, `epoll_wait`)
- **macOS:** `kqueue` (`kqueue`, `kevent`)

The Makefile auto-detects the OS with `uname` and compiles either `Server_linux.cpp` (epoll) or `Server.cpp` (kqueue). Both files implement the exact same `Server` class declared in `include/Server.hpp`.

```bash
# Show the OS detection in the Makefile
grep -n "UNAME\|Darwin\|SERVER_SRC" Makefile
```

### How does epoll work?

`epoll_create1(0)` creates a kernel object that we register file descriptors with. With `epoll_ctl(EPOLL_CTL_ADD, fd, EPOLLIN | EPOLLOUT, ...)` we tell the kernel "wake me up when this fd is ready for reading/writing". `epoll_wait()` blocks until at least one of those fds is ready and returns a list of events. The kernel does the readiness tracking — we never call `read()`/`write()` blindly.

`kqueue` is the BSD/macOS equivalent. The interface differs (filters, `EV_ADD`/`EV_DELETE`) but the principle is identical.

### Do we use only one epoll loop? How does it handle accept + read + write?

**Yes, one single epoll instance for the whole server.** Show:

```bash
grep -n "epoll_create1\|kqueue()" src/Server_linux.cpp src/Server.cpp
```

There is one fd `_kq` per Server object, created once in the constructor. The main loop in `Server::run()` is a single `epoll_wait` call:

```bash
grep -n "epoll_wait\|kevent(_kq, NULL" src/Server_linux.cpp src/Server.cpp
```

For each event the loop dispatches based on the fd:

- `_listen_fds` → `handleNewConnection()` (calls `accept`)
- `_cgi_pipe_fds` → `handleCgiRead()`
- `_cgi_write_fds` → `handleCgiWrite()`
- otherwise read event → `handleRead()` (calls `recv`)
- otherwise write event → `handleWrite()` (calls `send`)

Listen sockets are registered with `EPOLLIN` only. Client sockets switch between `EPOLLIN` (waiting for a request) and `EPOLLOUT` (response ready to send) using `EPOLL_CTL_MOD` — never both at once for one client, but the server as a whole monitors both directions simultaneously.

### Is there only one read OR one write per client per epoll cycle?

Yes. In `handleRead()` we call `recv()` exactly once with a 4096-byte buffer and return. In `handleWrite()` we call `send()` exactly once and return. We never loop reading or writing inside one event.

```bash
grep -n "recv(fd" src/Server_linux.cpp
grep -n "send(fd" src/Server_linux.cpp
```

### Show the path from epoll to read/write

```bash
# Linux version
sed -n '/^void Server::run/,/^}/p' src/Server_linux.cpp
sed -n '/^void Server::handleRead/,/^}/p' src/Server_linux.cpp
sed -n '/^void Server::handleWrite/,/^}/p' src/Server_linux.cpp
```

`run()` calls `epoll_wait`, dispatches by fd-type, and only then does `handleRead()` call `recv()` or `handleWrite()` call `send()`.

### Are read/recv/write/send errors handled correctly? (both -1 AND 0 checked)

Yes. We use `if (bytes <= 0)` everywhere, which catches both `-1` (error) and `0` (peer closed). On any non-positive return we close the client / pipe and clean up.

```bash
grep -n "recv\|send(fd\|read(pipe\|write(write_fd" src/Server_linux.cpp src/Cgi.cpp
grep -n -B1 -A3 "bytes <= 0\|bytes > 0" src/Server_linux.cpp
```

Specifically:

- `handleRead` (recv): `if (bytes <= 0) { removeClient(fd); return; }`
- `handleWrite` (send): `if (bytes <= 0) { removeClient(fd); return; }`
- `handleCgiRead` (read on pipe): `if (bytes > 0) { append } else { close pipe + waitpid }`
- `handleCgiWrite` (write on pipe): `if (bytes > 0) { advance offset } else { close pipe }`

### Is errno checked after read/recv/write/send?

**No.** This is forbidden by the subject. We never branch on `errno` after I/O. Show:

```bash
grep -n "errno" src/*.cpp include/*.hpp
```

You will see `errno` only used in the `epoll_wait` / `kevent` retry on `EINTR` (which is allowed — that is on the multiplexing call itself, not on a socket I/O call) and as `<cerrno>` includes. There is no `if (errno == ...)` after any `recv`, `send`, `read` or `write`.

### Are there any reads/writes that bypass epoll?

No fd that can block is read or written without first being polled. Listen sockets, client sockets, and CGI pipes are all registered with epoll. Regular disk files (config file, static HTML, error pages, uploads) use `std::ifstream` / `std::ofstream` — these are exempt by the subject ("regular disk files do not require readiness notifications").

### Does the project compile without re-link issues?

```bash
make fclean
make
make            # second invocation must do nothing
```

Second `make` should print `make: Nothing to be done for 'all'.`. The Makefile has correct dependencies and uses `.cpp.o` rules.

---

## 2. Configuration

### Status codes correctness

We map status codes in `ResponseBuilder::getStatusMessage`:

```bash
grep -n "case " src/Response.cpp
```

Codes implemented: 200, 201, 204, 301, 302, 400, 403, 404, 405, 413, 500, 501, 504. They follow RFC 7231.

### Multiple servers on different ports

`default.conf` has three server blocks: two on `127.0.0.1:8080` (different `server_name`s) and one on `127.0.0.1:9090`.

```bash
grep -n "port\|server_name" default.conf

# Demo:
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:9090/
```

Both should return `200 OK` with HTML content.

### Multiple servers with different hostnames (virtual hosts on the same port)

Both `localhost` and `example.com` listen on `8080`. We dispatch based on the `Host:` header in `Server::matchServerByHost`.

```bash
# Default server (localhost) — directory_listing OFF
curl -i --resolve localhost:8080:127.0.0.1 http://localhost:8080/

# Other vhost (example.com)
curl -i --resolve example.com:8080:127.0.0.1 http://example.com:8080/
```

You can also confirm by inspecting the `Server::matchServerByHost` logic:
```bash
sed -n '/matchServerByHost/,/^}/p' src/Server_linux.cpp
```

### Default error pages (custom 404)

Configured in `default.conf`:
```
error_page  404 /errors/404.html;
```

```bash
# Triggers our custom 404 (from www/errors/404.html)
curl -i http://127.0.0.1:8080/this-does-not-exist
```

If you want to prove the default fallback works, temporarily comment the `error_page` line in `default.conf` and restart — you'll get a server-generated `<html>404 Not Found</html>` instead.

### Client body size limit

Server-level default: `max_body_size 200M`. Location `/post_body` overrides with `max_body_size 100`.

```bash
# Smaller than 100 bytes — accepted, body is echoed back
curl -i -X POST -H "Content-Type: text/plain" --data "small" http://127.0.0.1:8080/post_body

# Larger than 100 bytes — rejected with 413 Payload Too Large
curl -i -X POST -H "Content-Type: text/plain" \
  --data "BODY IS HERE this string is much longer than one hundred bytes so it should trigger 413 Payload Too Large from the server" \
  http://127.0.0.1:8080/post_body
```

### Routes pointing to different directories

Look at the `location` blocks in `default.conf`:

| Route        | Root                  |
|--------------|-----------------------|
| `/`          | `./www`               |
| `/directory` | `./www/YoupiBanane`   |
| `/upload`    | `./www/uploads`       |
| `/cgi-bin`   | `./www/cgi-bin`       |

```bash
curl -i http://127.0.0.1:8080/
curl -i http://127.0.0.1:8080/directory/
ls www/uploads
```

### Default file for a directory (index)

```
location / { index index.html; }
```

```bash
# Serves www/index.html, not a directory listing
curl -i http://127.0.0.1:8080/
```

### Accepted methods per route

`/upload` accepts GET, POST, DELETE. `/post_body` accepts only POST. `/old` accepts only GET (it's a redirect).

```bash
# DELETE on /post_body — should be 405 Method Not Allowed
curl -i -X DELETE http://127.0.0.1:8080/post_body

# DELETE on /upload/somefile — first upload then delete
echo "test content" > /tmp/del_test.txt
curl -i -X POST -F "file=@/tmp/del_test.txt" http://127.0.0.1:8080/upload
curl -i -X DELETE http://127.0.0.1:8080/upload/del_test.txt
ls www/uploads
```

---

## 3. Basic checks

### GET / POST / DELETE

```bash
# GET
curl -i http://127.0.0.1:8080/

# POST (echoed back)
curl -i -X POST -H "Content-Type: text/plain" --data "hello world" http://127.0.0.1:8080/post_body

# DELETE
echo "deleteme" > /tmp/dm.txt
curl -i -X POST -F "file=@/tmp/dm.txt" http://127.0.0.1:8080/upload
curl -i -X DELETE http://127.0.0.1:8080/upload/dm.txt
```

### Unknown method does not crash

```bash
curl -i -X BANANA http://127.0.0.1:8080/
# Expect: 501 Not Implemented (defined in RequestParser::isValidMethod)
# Server keeps running.

# Try a few more
curl -i -X PURGE http://127.0.0.1:8080/
curl -i -X TRACE http://127.0.0.1:8080/
```

### Status codes

| Test                                  | Expected status        |
|---------------------------------------|------------------------|
| GET existing file                     | `200 OK`               |
| Successful upload                     | `201 Created`          |
| Successful DELETE                     | `204 No Content`       |
| GET non-existent                      | `404 Not Found`        |
| GET `/old` (redirect)                 | `301 Moved Permanently`|
| Wrong method on `/post_body`          | `405 Method Not Allowed`|
| Body > location max_body_size         | `413 Payload Too Large`|
| Unknown HTTP method                   | `501 Not Implemented`  |
| CGI infinite loop                     | `504 Gateway Timeout`  |

### Upload a file and retrieve it

```bash
# Make a unique file
echo "Hello $(date +%s)" > /tmp/upload_demo.txt

# Upload
curl -i -X POST -F "file=@/tmp/upload_demo.txt" http://127.0.0.1:8080/upload

# Confirm it landed on the server
ls -la www/uploads/

# Download it back
curl -i http://127.0.0.1:8080/upload/upload_demo.txt
```

---

## 4. CGI

### CGI works (Python and `cgi_tester`)

`default.conf` declares three handlers in `/cgi-bin`:
```
cgi .py  /usr/bin/python3;
cgi .php /usr/bin/php-cgi;
cgi .bla ./www/cgi-bin/cgi_tester;
```

```bash
# Make sure scripts are executable
chmod +x www/cgi-bin/*.py

# Python CGI — GET
curl -i "http://127.0.0.1:8080/cgi-bin/hello.py?name=Gio"

# Show all environment variables that reach the CGI
curl -i http://127.0.0.1:8080/cgi-bin/env_debug.py
```

You should see `REQUEST_METHOD`, `QUERY_STRING`, `CONTENT_LENGTH`, `CONTENT_TYPE`, `PATH_INFO`, `SERVER_PROTOCOL`, `GATEWAY_INTERFACE`, `SERVER_SOFTWARE`, all `HTTP_*` headers. See `CgiHandler::buildEnv()`:

```bash
sed -n '/buildEnv/,/^}/p' src/Cgi.cpp
```

### CGI runs in the correct directory (relative path access)

Before `execve` we `chdir(getScriptDir())`. The binary path is rewritten with `../` prefixes so it still resolves from the new working directory:

```bash
sed -n '/adjustBinaryForChdir/,/^}/p' src/Cgi.cpp
sed -n '/chdir/,/execve/p' src/Cgi.cpp
```

Demo: the `cgi_tester` itself does relative-file-access checks. If `chdir` is wrong, it fails:
```bash
curl -i http://127.0.0.1:8080/directory/youpi.bla
```

### CGI with GET and POST

```bash
# GET
curl -i "http://127.0.0.1:8080/cgi-bin/hello.py?greeting=hi"

# POST — the official cgi_tester echoes uppercased stdin
curl -i -X POST -d "HalloWebserv" http://127.0.0.1:8080/cgi-bin/test.bla
# Expect body: HALLOWEBSERV
```

### Error handling (broken script, infinite loop)

```bash
# 1. Infinite loop — server kills CGI after CGI_TIMEOUT (120s) and returns 504
#    To see it fast, temporarily lower CGI_TIMEOUT in include/Server.hpp to 5
#    and rebuild. Otherwise just demonstrate that the server stays responsive
#    while the loop runs:
curl -i --max-time 3 http://127.0.0.1:8080/cgi-bin/infinite.py &
sleep 1
curl -i http://127.0.0.1:8080/                     # other requests still work
wait

# 2. Script that doesn't exist
curl -i http://127.0.0.1:8080/cgi-bin/no_such.py   # 404

# 3. Script with a Python syntax error
cat > www/cgi-bin/broken.py << 'EOF'
#!/usr/bin/env python3
this is not valid python
EOF
chmod +x www/cgi-bin/broken.py
curl -i http://127.0.0.1:8080/cgi-bin/broken.py    # 500 / parsed CGI error
rm www/cgi-bin/broken.py
```

The server **never crashes** during these tests — show that other requests keep working in parallel.

### How the timeout is implemented

```bash
grep -n "CGI_TIMEOUT\|cgi_start" include/Server.hpp src/Server_linux.cpp
sed -n '/checkTimeouts/,/^}/p' src/Server_linux.cpp
```

In `checkTimeouts()` (called every loop iteration after `epoll_wait`), if `now - cgi_start > CGI_TIMEOUT` we `kill(SIGKILL)` the child, `waitpid` it, close the pipe fds, and queue a `504 Gateway Timeout` response.

---

## 5. Browser test

```bash
./webserv default.conf
```

Open Firefox / Chromium and navigate to `http://127.0.0.1:8080/`. Open DevTools (F12) → **Network** tab → reload.

Things to point out to the evaluator:
- **Request headers**: `Host`, `User-Agent`, `Accept`, `Connection: keep-alive`.
- **Response headers**: `HTTP/1.1 200 OK`, `Content-Type: text/html`, `Content-Length`, `Date`, `Server: webserv/1.0`, `Connection: keep-alive`.
- **Static assets** (HTML) load correctly — fully static website OK.
- **Wrong URL**: navigate to `http://127.0.0.1:8080/no/such/page` — custom 404 page is rendered.
- **Directory listing**: navigate to `http://127.0.0.1:9090/listing/` — shows the file index (server on port 9090 has `directory_listing on`).
- **Redirected URL**: navigate to `http://127.0.0.1:8080/old` — browser follows the 301 to `http://127.0.0.1:8080/`.

---

## 6. Port issues

### Multiple ports, different content

`default.conf` has port 8080 (directory listing OFF) and port 9090 (directory listing ON).

```bash
curl -s http://127.0.0.1:9090/listing/ | head -5     # HTML directory listing
curl -i http://127.0.0.1:8080/listing/                # 403 / 404 (no listing, no index)
```

### Same port + same host + same server_name twice → must fail

Test:
```bash
cat > /tmp/dup.conf << 'EOF'
server { host 127.0.0.1; port 8080; server_name foo; location / { root ./www; methods GET; } }
server { host 127.0.0.1; port 8080; server_name foo; location / { root ./www; methods GET; } }
EOF
./webserv /tmp/dup.conf
```

Expected: `Fatal error: Duplicate server combination: 127.0.0.1:8080 (foo)`. See `ConfigParser::parse()`:
```bash
grep -n "Duplicate server combination" src/ConfigParser.cpp
```

### Multiple servers sharing a port (different server_names)

The three blocks in `default.conf` share port 8080 between two of them. The server creates **one listen socket per host:port pair** and dispatches by `Host:` header (`matchServerByHost`). Demo earlier in section 2.

If one server block has a broken root, only requests targeting that vhost fail — other vhosts keep working. Demo:
```bash
# Edit default.conf: change example.com's root to /nonexistent/, then restart
# curl localhost still works, curl example.com gives 404
```

---

## 7. Siege & stress test

### Availability ≥ 99.5% on a simple GET

```bash
# Long-running benchmark — let it run for a minute, then Ctrl+C
siege -b -t 1M http://127.0.0.1:8080/
```

Look at the `Availability:` line in the report — should be `99.5+%` or 100%. If you want a quicker check:
```bash
siege -b -t 30S http://127.0.0.1:8080/
```

### No memory leak (RSS doesn't grow indefinitely)

```bash
# Terminal A
./webserv default.conf

# Terminal B — get the PID
pgrep -f "./webserv default.conf"

# Terminal C — start a stress test
siege -b -t 2M http://127.0.0.1:8080/

# Terminal B — watch RSS in KB
watch -n 1 "ps -o pid,rss,vsz,cmd -p $(pgrep -f './webserv default.conf')"
```

RSS should stabilize after a short warm-up. Confirm with valgrind for a short run:

```bash
# Stop the server, then:
valgrind --leak-check=full --show-leak-kinds=all --suppressions=/dev/null \
  ./webserv default.conf &
VPID=$!
sleep 2
siege -b -t 30S http://127.0.0.1:8080/
kill -INT $VPID
wait
```

Expect `definitely lost: 0 bytes` and `indirectly lost: 0 bytes`. (Valgrind may show "still reachable" — that is the OS cleaning up at exit and is not a leak.)

### No hanging connections

```bash
# Number of established connections to webserv before, during and after siege
ss -tan state established '( sport = :8080 )' | wc -l
```

After siege finishes and the keep-alive timeout (`TIMEOUT_SEC = 120`) elapses, this should drop back to 0.

### Indefinite siege

```bash
siege -b http://127.0.0.1:8080/   # no -t — runs until Ctrl+C
```

Server must keep responding for as long as siege keeps running.

---

## 8. Bonus

### Multiple CGI types

`/cgi-bin` already accepts three extensions:
```
cgi .py  /usr/bin/python3;
cgi .php /usr/bin/php-cgi;
cgi .bla ./www/cgi-bin/cgi_tester;
```

Demo:
```bash
# Python
curl -i http://127.0.0.1:8080/cgi-bin/hello.py
# Custom binary
curl -i -X POST -d "TEST" http://127.0.0.1:8080/cgi-bin/test.bla
# PHP (only if php-cgi is installed)
which php-cgi && echo '<?php echo "php works"; ?>' > www/cgi-bin/hello.php \
  && curl -i http://127.0.0.1:8080/cgi-bin/hello.php
```

The selection is done in `ResponseBuilder::matchCgi` — for each location the server iterates its `cgi` vector and matches by file extension.

### Cookies / sessions

**Not implemented.** If asked, mention it explicitly.

---

## Quick reference — code locations

| Topic                          | File                               |
|--------------------------------|------------------------------------|
| Single epoll loop              | `src/Server_linux.cpp::run()`      |
| Single kqueue loop             | `src/Server.cpp::run()`            |
| Accept new client              | `handleNewConnection()`            |
| Read from client (recv)        | `handleRead()`                     |
| Write to client (send)         | `handleWrite()`                    |
| Read from CGI pipe             | `handleCgiRead()`                  |
| Write to CGI pipe              | `handleCgiWrite()`                 |
| Timeout / 504                  | `checkTimeouts()`                  |
| Config parser                  | `src/ConfigParser.cpp`             |
| HTTP request parser            | `src/Request.cpp::RequestParser`   |
| Routing + status codes         | `src/Response.cpp::ResponseBuilder`|
| CGI fork/exec/env              | `src/Cgi.cpp`                      |
| Chunked transfer decoding      | `Request.cpp::parseChunked`        |
| Multipart upload parsing       | `Response.cpp::handleUpload`       |
| Virtual host dispatch          | `Server*::matchServerByHost`       |

---

## "Gotchas" — pre-empt these questions

1. **"Why two Server files?"** macOS uses kqueue, Linux uses epoll. Same class, two implementations. Picked by the Makefile via `uname`.
2. **"Why don't you use `inet_pton`?"** It's not on the allowed function list. We wrote `parseIPv4()` ourselves.
3. **"Why `swap()` everywhere?"** `std::string::swap` is O(1) — avoids copying potentially large request bodies.
4. **"Why a `COMPACT_THRESHOLD`?"** `string::erase(0, n)` is O(remaining). On a long-lived keep-alive connection with many small writes, repeatedly erasing from the front is O(n²). We only compact when the wasted prefix exceeds 64 KB.
5. **"Why early CGI start?"** For large POST bodies we start the CGI as soon as headers are parsed and stream the body into the CGI's stdin pipe. Otherwise we'd buffer the whole upload in memory before forking.
6. **"Why `signal(SIGPIPE, SIG_IGN)`?"** A client closing during a write would otherwise kill the server with SIGPIPE. We get `send()` returning `-1` instead, which `handleWrite` cleans up.
7. **"errno after recv?"** Never — only `if (bytes <= 0)`. The only `errno` use is on `epoll_wait`/`kevent` for the `EINTR` retry, which is on the multiplexing call, not on socket I/O.
