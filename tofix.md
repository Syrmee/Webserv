# Webserv — Evaluation Readiness: Complete Bug Fix & Feature Plan

This plan covers every issue found in the codebase that would cause points to be deducted during the 42 evaluation, organized by severity.

---

## User Review Required

> [!CAUTION]
> **Issue #1 — `errno` checked after `recv()`/`send()` → GRADE = 0**
> In [Connection.cpp:94](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.cpp#L94) and [line 138](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.cpp#L138), `perror()` is called after `recv()` and `send()`. `perror()` reads `errno` — the eval sheet says: *"If errno is checked after read/recv/write/send, the grade is 0."*

> [!CAUTION]
> **Issue #2 — Double-close bug in `closeClient()` → potential crash / FD corruption**
> [closeClient()](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp#L40-L55) calls `close(fd)` on line 44, then `delete conns[fd]` on line 49 triggers `~Connection()` which calls `::close(fd_)` **again** (because `closed_` is still `false`). Between the two closes, the OS may reassign that FD to a new connection — the second close would then silently kill the new connection. This is a data-corruption / crash bug.

> [!CAUTION]
> **Issue #3 — `EAGAIN`/`EWOULDBLOCK` not handled on non-blocking sockets**
> When `recv()` returns -1, the code immediately closes the connection ([Connection.cpp:93-96](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.cpp#L93-L96)). But for non-blocking sockets, `EAGAIN` is **normal** — it means "no data available right now, try later." Same issue on CGI pipe `read()` at [webserv.cpp:718](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp#L718) and `write()` at [webserv.cpp:772](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp#L772). However, since we poll() before every read/write, EAGAIN should be very rare. But the eval says *"if an error is returned, the client is removed"* — we need to handle EAGAIN gracefully (not close) while still closing on real errors. The key constraint is: we **cannot check errno** after the call. Solution: we know we only read/write after poll() readiness, so any -1 that isn't EAGAIN is a real error. Since we can't distinguish without errno, we should **not close on -1 from recv/send at all** — instead return -1 and let the caller decide (the next poll iteration will report POLLERR/POLLHUP if the socket is truly dead).

> [!WARNING]
> **Issue #4 — No CGI timeout mechanism**
> If a CGI script enters an infinite loop, the server leaks the child process and pipe FDs forever. The eval explicitly tests this: *"You can use a script containing an infinite loop."*

> [!WARNING]
> **Issue #5 — No client connection timeout**
> The subject says *"A request to your server should never hang indefinitely."* No mechanism to disconnect idle/slow clients (slowloris attack vector).

> [!WARNING]
> **Issue #6 — Missing mandatory CGI environment variables**
> [CgiHandler.cpp:initEnv()](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/CgiHandler.cpp#L49-L110) is missing several **RFC 3875 mandatory** variables:
> - `REQUEST_METHOD` — **critical**, most CGI scripts branch on this
> - `SERVER_NAME` — required
> - `SERVER_PORT` — required
> - `REMOTE_ADDR` — required (can use "127.0.0.1" as placeholder)

> [!WARNING]
> **Issue #7 — CGI orphan cleanup on error paths**
> If a CGI write pipe errors out ([webserv.cpp:780-785](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp#L780-L785)), the write pipe FD is cleaned but the read pipe FD stays in `cgi_read_map` pointing at a Connection whose CGI handler is in a broken state. Similarly for `POLLERR|POLLHUP` at [line 798-803](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp#L798-L803).

> [!WARNING]
> **Issue #8 — `cgiBytesWritten_` not reset between keep-alive requests**
> When a connection is reused (keep-alive), `cgiBytesWritten_` and `cgiOutput_` are never reset. A second CGI request on the same connection will start writing from the wrong offset.

> [!IMPORTANT]
> **Issue #9 — `shouldKeepAlive()` redundant condition** at [utils.cpp:147](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/utils.cpp#L147)
> `status >= 500 || status >= 501` — second condition is unreachable. Works correctly but looks like a copy-paste error.

> [!IMPORTANT]
> **Issue #10 — Debug `cout` statements left in production code**
> Multiple `std::cout` calls in [Request.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Request.cpp) (lines 87, 182, 207, 216, 302, 310) and [Request_header.cpp:159](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Request_header.cpp#L159). These pollute stdout during evaluation.

> [!IMPORTANT]
> **Issue #11 — Fragile status code detection by string search**
> [webserv.cpp:530-541](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp#L530-L541) detects response status by searching for `" 405 "` etc. in the output buffer. Could match body content. Also has duplicate checks for 400 and 500.

> [!IMPORTANT]
> **Issue #12 — Copy constructors/assignment operators not disabled**
> `Connection`, `Server`, and `CgiHandler` hold raw pointers/FDs but have no private copy constructor or assignment operator. Accidental copy = double-free/double-close.

---

## Open Questions

> [!IMPORTANT]
> **Q1:** Do you want virtual host support (routing based on `Host:` header to different server blocks sharing the same `host:port`)? The subject says it's optional. Your current code binds separate server blocks to separate `host:port` pairs, which is valid.

> [!IMPORTANT]
> **Q2:** Do you have a static website ready for the eval demo? The eval requires serving "a fully static website" from the browser. Should I create a simple demo site under `/tmp/www/`?

> [!IMPORTANT]
> **Q3:** There's no `README.md` in the project. The subject requires one with specific sections. Should I create it? (If so, what are your 42 logins?)

---

## Proposed Changes

### 1. Critical: Remove errno checks after recv/send

#### [MODIFY] [Connection.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.cpp)

Remove `perror()` after `recv()` and `send()`. Also handle the EAGAIN case gracefully: since we can't check errno, and we know poll() gave us readiness, treat -1 as a non-fatal retry (return 0 to signal "try again") rather than immediately closing:

```diff
 int Connection::readFromSocket()
 {
     if (inBuffer_.size() > MAX_HEADER_SIZE && inBuffer_.find("\r\n\r\n") == std::string::npos)
         throw Request::ParseError(431);

     char buf[8192];
     ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);

     if (n > 0)
     {
         inBuffer_.append(buf, n);
         return static_cast<int>(n);
     }
     else if (n == 0)
         return 0;

-    perror("recv()");
-    closeNow();
     return -1;
 }

 int Connection::writeToSocket()
 {
     // ... existing empty check ...
     ssize_t n = ::send(fd_, dataPtr, remaining, 0);

     if (n >= 0)
     {
         // ... offset tracking ...
         return static_cast<int>(n);
     }

-    perror("send()");
-    closeNow();
     return -1;
 }
```

---

### 2. Critical: Fix double-close in closeClient()

#### [MODIFY] [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp)

Remove the raw `close(fd)` call and let `~Connection()` handle it via `closeNow()`:

```diff
 void closeClient(int fd, std::map<int, Connection*>& conns,
                  std::map<int, const ServerConfig*>& cfg_map,
                  std::vector<int>& fds_to_remove)
 {
-    close(fd);
-
     // SAFETY: Check if it exists before deleting
     if (conns.find(fd) != conns.end())
     {
+        conns[fd]->closeNow();
         delete conns[fd]; // Calls ~Connection()
         conns.erase(fd);
     }
+    else
+    {
+        close(fd); // Orphan FD with no Connection (safety fallback)
+    }

     cfg_map.erase(fd);
     fds_to_remove.push_back(fd);
 }
```

---

### 3. Critical: Handle recv/send -1 properly in callers

#### [MODIFY] [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp)

In `handleClient()`, treat `readFromSocket()` returning -1 as a close signal:

```diff
         try {
             int bytesRead = conn->readFromSocket();
-            if (bytesRead == 0 && conn->in().empty() && conn->out().empty())
+            if (bytesRead < 0)
+                return false; // Socket error, close client
+            if (bytesRead == 0 && conn->in().empty() && conn->out().empty())
                 return false;
```

For CGI pipe `read()` at line ~718, handle -1 (could be EAGAIN — just ignore and try next poll iteration):

```diff
 ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);

 if (bytes > 0) {
     conn->getCgiOutput().append(buffer, bytes);
 } else if (bytes == 0) {
     // EOF — process CGI response
     // ... existing code ...
-}
+} else {
+    // bytes == -1: likely EAGAIN, just wait for next poll
+    // If pipe is truly broken, POLLERR/POLLHUP will fire next iteration
+}
```

For CGI pipe `write()` at line ~772, same approach:

```diff
 ssize_t written = write(fd, body.c_str() + bytesAlreadySent, remaining);

 if (written > 0)
 {
     conn->addCgiBytesWritten(written);
 }
-else if (written == -1)
+else if (written <= 0)
 {
-    // FATAL ERROR: Pipe is broken
-    close(fd);
-    cgi_write_map.erase(fd);
-    fds_to_remove.push_back(fd);
-    continue;
+    // written == 0 or -1: could be EAGAIN, just retry next poll
+    // If pipe is truly broken, POLLERR will fire next iteration
 }
```

---

### 4. CGI Timeout System

#### [MODIFY] [Connection.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp)

```diff
+#include <ctime>

 class Connection
 {
 private:
     // ...
     CgiHandler* cgiHandler_;
     std::string cgiOutput_;
     size_t cgiBytesWritten_;
+    time_t cgiStartTime_;

 public:
     // ...
+    time_t getCgiStartTime() const { return cgiStartTime_; }
+    void   setCgiStartTime(time_t t) { cgiStartTime_ = t; }
 };
```

#### [MODIFY] [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp)

1. Define `CGI_TIMEOUT_SECONDS` constant (e.g., 10 seconds)
2. When launching CGI (~line 338): `conn->setCgiStartTime(time(NULL))`
3. In the main event loop, after poll returns, sweep `cgi_read_map`:
   - If `time(NULL) - conn->getCgiStartTime() > CGI_TIMEOUT_SECONDS`:
     - Kill child, close both pipe FDs, erase from maps, push to `fds_to_remove`
     - Set `conn->out()` to `buildError(504, ...)` (Gateway Timeout)
     - Delete CGI handler, set to NULL

---

### 5. Client Connection Timeout

#### [MODIFY] [Connection.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp)

```diff
 private:
     // ...
+    time_t lastActivity_;

 public:
     // ...
+    time_t getLastActivity() const { return lastActivity_; }
+    void   touchActivity() { lastActivity_ = time(NULL); }
```

#### [MODIFY] [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp)

1. Define `CLIENT_TIMEOUT_SECONDS` constant (e.g., 60 seconds)
2. On new connection: `new_conn->touchActivity()`
3. On successful read/write: `conn->touchActivity()`
4. After the poll-fd iteration loop, sweep `connections`:
   - If `time(NULL) - conn->getLastActivity() > CLIENT_TIMEOUT_SECONDS` and the connection has no active CGI:
     - `closeClient(fd, ...)`

---

### 6. Missing CGI Environment Variables

#### [MODIFY] [CgiHandler.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/CgiHandler.cpp)

Add mandatory RFC 3875 variables to `initEnv()`:

```diff
 void CgiHandler::initEnv(const Request& req)
 {
     envMap_["GATEWAY_INTERFACE"] = "CGI/1.1";
     envMap_["SERVER_PROTOCOL"]   = req.getVersion();
     envMap_["SERVER_SOFTWARE"]   = "webserv/0.1";
     envMap_["QUERY_STRING"]      = req.getQuery();
+    envMap_["REQUEST_METHOD"]    = req.getMethod();
+    envMap_["SERVER_NAME"]       = "localhost";
+    envMap_["SERVER_PORT"]       = "8080"; // ideally from config
+    envMap_["REMOTE_ADDR"]       = "127.0.0.1";
+    envMap_["REMOTE_HOST"]       = "127.0.0.1";
     // ... rest of existing code ...
```

To get the actual port, we need to pass the `ServerConfig*` to CgiHandler (currently only `Location&` is passed). We can either:
- **(A)** Pass `srvCfg->port` as an additional constructor arg
- **(B)** Hardcode reasonable defaults

I recommend **(A)** for correctness.

#### [MODIFY] [CgiHandler.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/CgiHandler.hpp)

Add `port` and `serverName` parameters to constructor.

---

### 7. CGI Orphan Cleanup

#### [MODIFY] [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp)

When a CGI write pipe errors out (POLLERR/POLLHUP at line ~798), also clean the read pipe:

```diff
 // CASE 3: CGI write pipe error
 else if (revents & (POLLERR | POLLHUP))
 {
     close(fd);
     cgi_write_map.erase(fd);
     fds_to_remove.push_back(fd);
+    // Clean the read pipe too
+    Connection* conn = cgi_write_map.count(fd) ? cgi_write_map[fd] : NULL;
+    if (conn && conn->getCgiHandler()) {
+        int readFd = conn->getCgiHandler()->getReadFd();
+        if (cgi_read_map.count(readFd)) {
+            close(readFd);
+            cgi_read_map.erase(readFd);
+            fds_to_remove.push_back(readFd);
+        }
+        conn->out() = buildError(502, conn->request(), NULL);
+        delete conn->getCgiHandler();
+        conn->setCgiHandler(NULL);
+    }
 }
```

---

### 8. Reset CGI State Between Requests

#### [MODIFY] [Connection.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp) / [Connection.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.cpp)

Add a `resetCgiState()` method and call it when setting up a new CGI:

```cpp
void Connection::resetCgiState() {
    cgiBytesWritten_ = 0;
    cgiOutput_.clear();
}
```

Call this in webserv.cpp before `setCgiHandler()`.

---

### 9. Fix shouldKeepAlive Redundancy

#### [MODIFY] [utils.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/utils.cpp)

```diff
-    if (status == 400 || status == 408 || status == 413 ||
-        status == 414 || status == 431 || status >= 500 || status >= 501)
+    if (status == 400 || status == 408 || status == 413 ||
+        status == 414 || status == 431 || status >= 500)
         return false;
```

---

### 10. Remove Debug Output

#### [MODIFY] [Request.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Request.cpp)

Remove all `std::cout` at lines 87, 182, 207, 216, 302, 310.

#### [MODIFY] [Request_header.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Request_header.cpp)

Remove `std::cout << "non valid header"` at line 159.

---

### 11. Store Response Status in Connection

#### [MODIFY] [Connection.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp)

```diff
 private:
     // ...
+    int responseStatus_;

 public:
     // ...
+    int  getResponseStatus() const { return responseStatus_; }
+    void setResponseStatus(int s) { responseStatus_ = s; }
```

#### [MODIFY] [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp)

Set `conn->setResponseStatus(code)` when building error/success responses. In the POLLOUT section, use `conn->getResponseStatus()` instead of string searching.

---

### 12. Disable Copy for RAII Classes

#### [MODIFY] [Connection.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp), [Server.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Server.hpp), [CgiHandler.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/CgiHandler.hpp)

Add private, unimplemented copy constructor and assignment operator (C++98 idiom):

```cpp
private:
    Connection(const Connection&);
    Connection& operator=(const Connection&);
```

---

### 13. Makefile Dependency Tracking

#### [MODIFY] [Makefile](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Makefile)

```diff
+DEPS = $(SRCS:.cpp=.d)
+
 %.o: %.cpp
-	$(CXX) $(CXXFLAGS) -c $< -o $@
+	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@
+
+-include $(DEPS)

 clean:
-	rm -f $(OBJS)
+	rm -f $(OBJS) $(DEPS)
```

---

### 14. Missing Files

#### [NEW] README.md

At project root, with subject-required sections:
- Italicized first line with logins
- Description, Instructions, Resources (including AI usage)

#### [NEW] Demo website + CGI scripts

Static files in `/tmp/www/` for eval demo. Setup script or fixture included.

#### [MODIFY] [eval.conf](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/eval.conf)

Add CGI location block for Python scripts.

---

## Summary Table

| # | File(s) | Issue | Severity |
|---|---------|-------|----------|
| 1 | [Connection.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.cpp) | `perror()` after recv/send | **GRADE=0** |
| 2 | [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp) | Double-close in closeClient | **CRASH** |
| 3 | [Connection.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.cpp) + callers | EAGAIN closes connection | **CRASH** |
| 4 | [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp) + [Connection.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp) | No CGI timeout | **CRITICAL** |
| 5 | [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp) + [Connection.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp) | No client timeout | **CRITICAL** |
| 6 | [CgiHandler.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/CgiHandler.cpp) | Missing REQUEST_METHOD etc. | **CRITICAL** |
| 7 | [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp) | CGI orphan cleanup | **CRITICAL** |
| 8 | [Connection](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp) | CGI state not reset | MEDIUM |
| 9 | [utils.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/utils.cpp) | Redundant keepAlive condition | LOW |
| 10 | [Request.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Request.cpp), [Request_header.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Request_header.cpp) | Debug cout | LOW |
| 11 | [webserv.cpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/webserv.cpp), [Connection.hpp](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Connection.hpp) | Fragile status string search | LOW |
| 12 | Headers | Missing copy protection | LOW |
| 13 | [Makefile](file:///wsl.localhost/Ubuntu/home/eperrier/Code/Webserv/Makefile) | No header deps | LOW |
| 14 | New files | README.md, demo content | **REQUIRED** |

---

## Verification Plan

### Automated Tests
1. `make re` — verify clean compilation with `-Wall -Wextra -Werror -std=c++98`
2. `grep -rn "perror\|errno" *.cpp *.hpp` — verify no errno usage after read/write
3. `python3 eval_test.py` — verify all existing tests pass
4. CGI timeout test: script with `while True: pass`, verify 504 after timeout
5. Client timeout test: `telnet` + send nothing, verify disconnection
6. `siege -b -c 100 -t 30S http://127.0.0.1:8080/` — verify >99.5% availability
7. Duplicate port config: same `host:port` twice → verify startup error
8. All HTTP methods: GET, POST, DELETE, HEAD, UNKNOWN
9. Chunked encoding: `curl -X POST -H "Transfer-Encoding: chunked" -d @file`
10. CGI with GET and POST methods
11. CGI infinite loop → verify no server hang, 504 returned
12. Memory monitoring during siege (watch RSS with `top`)

### Manual Verification
- Browser → `http://127.0.0.1:8080/` → verify static website renders
- Browser dev tools → check response headers
- Directory listing, redirects, file upload/download/delete via browser
- Valgrind or similar for leak detection