# Understanding Our C++98 Webserver: A Complete Guide

 We will explain the code using a simple analogy: **The Highly Efficient Restaurant**.

---

## 1. The Big Picture: The Restaurant Analogy

Imagine a restaurant with only **one waiter** (our single-threaded program). 

If the waiter takes an order, walks to the kitchen, waits 20 minutes for the food to cook, brings it to the table, and *only then* goes to the next table, the restaurant will fail. This is a **blocking** architecture.

To succeed, our single waiter must use a **non-blocking, event-driven** approach. They check Table 1 (are they ready to order?), then check Table 2 (do they need water?), then check the kitchen (is the food ready?). They never stand still. They only interact with a table or the kitchen when an *event* occurs.

This is exactly how our webserver works using the `poll()` system call.

---

## 2. Phase 1: The Blueprint (`Config.cpp` & `Config.hpp`)

Before the restaurant opens, the manager sets the rules: What are the opening hours? What is on the menu? Which tables allow smoking?

In our code, this is handled by the **Configuration Parser**. 

1.  **Tokenization (`tokenize`)**: The server reads the `.conf` file and breaks it into meaningful chunks (tokens) like `server`, `{`, `listen`, `8080`, `}`. It ignores extra spaces and comments.
2.  **The State Machine (`parse`)**: We loop through the tokens using a state machine. 
    *   If we see `server {`, we enter the **Server Scope**. We start configuring things like the `host`, `port`, and default `error_pages`.
    *   If we see `location /images {` inside a server, we enter the **Location Scope** (Route rules). We set up specific rules for that path, like where the files are (`root`), if we can upload files (`upload_store`), or if we should run scripts (`cgi_pass`).

The result is a structured `std::vector<ServerConfig>` that acts as the absolute truth for how the server behaves.

---

## 3. Phase 2: Opening the Doors (`Server.cpp`)

With the rules set, we need to open the physical doors to let clients in. For each `ServerConfig` block, we create a `Server` object which handles the low-level Networking magic.

1.  **`socket()`**: We ask the OS for an endpoint for communication.
2.  **`setsockopt(SO_REUSEADDR)`**: We tell the OS "If this server crashes, let me restart and use this port immediately without waiting for a timeout."
3.  **`fcntl(O_NONBLOCK)`**: **CRITICAL STEP.** We configure the socket to be non-blocking. If we try to accept a client and no one is there, the OS immediately returns an error (`EAGAIN`) instead of freezing our program.
4.  **`bind()` & `listen()`**: We attach the socket to an IP and Port (e.g., `127.0.0.1:8080`) and tell the OS to start queuing up incoming connection requests.

---

## 4. Phase 3: The Brain of the Operation (`webserv.cpp` Event Loop)

This is the core of our program—our highly efficient waiter. 

We use `poll()`. We give the Operating System a list of file descriptors (`poll_fds`) and say: *"Put my program to sleep. Wake me up when ANY of these descriptors have data to read, or are ready to be written to."*

The `poll_fds` vector tracks three types of entities:
1.  **Listening Sockets:** Waiting for new clients to connect.
2.  **Client Sockets:** Waiting to read their HTTP requests or send them HTTP responses.
3.  **CGI Pipes:** Waiting to read output from a running script (like Python or PHP) or send it POST data.

### The Loop Cycle:
1.  **Wait:** `poll()` blocks until an event happens.
2.  **Check Listeners:** If a listener has `POLLIN`, a new client is at the door! We call `accept()`, wrap their socket in a `Connection` object, make it non-blocking, and add it to `poll_fds`.
3.  **Check CGI:** If a CGI script has finished thinking and printed output (`POLLIN`), we read it. If it needs POST data (`POLLOUT`), we write to it.
4.  **Check Clients:** If an existing client sent data (`POLLIN`), we read it. If we have a response ready to send to a client (`POLLOUT`), we send it.
5.  **Cleanup:** If anyone disconnected, we safely remove them using our `fds_to_remove` deferred-deletion list to prevent our program from crashing while looping.

---

## 5. Phase 4: Understanding the Client (`Request.cpp`)

When a client sends data, it arrives as raw text. The `Request` class is responsible for decoding this text into a structured format we can use.

1.  **The Request Line:** We extract the Method (`GET`, `POST`), the Path (`/index.html`), and the Version (`HTTP/1.1`).
2.  **The Headers:** We read `Key: Value` pairs (like `Host: localhost`, `User-Agent: Mozilla`). We validate these for security (preventing "Request Smuggling" by checking for forbidden characters).
3.  **The Body:** 
    *   If it's a simple request, we read until we match the `Content-Length`.
    *   If it is **Chunked Encoding** (`Transfer-Encoding: chunked`), we use a special state machine (`parseChunkedBody`) to decode the data on the fly. It reads the hex size, reads the data block, reads the trailing `\r\n`, and repeats until it hits a `0` size chunk.

---

## 6. Phase 5: Doing the Work (Routing & CGI)

Once the request is fully parsed, we decide what to do inside `handleClient()` in `webserv.cpp`.

First, we match the request path against our `Location` rules to find the "Best Match" (the most specific route).

### Scenario A: Static Files & Autoindex
If they want a file (e.g., `GET /style.css`), we resolve the full path on the hard drive using `resolveFullPath()`. 
*   If it's a file, we open it, figure out the MIME type (e.g., `text/css`), and send it.
*   If it's a directory and `autoindex` is on, we use the `<dirent.h>` library to read the directory contents and generate an HTML page with clickable links on the fly!

### Scenario B: Uploading Files
If they send a `POST` request to an upload route, we extract the filename from the URI, create a new file in the `upload_store` directory, and write the binary request body into it.

### Scenario C: The Kitchen (CGI - Common Gateway Interface)
If the file ends with a configured CGI extension (e.g., `.php`), we can't just send the text of the code; we have to *execute* the code. This happens in `CgiHandler.cpp`.

1.  **The Pipes:** We create two unidirectional pipes. One for the Server to send the request body to the Script (`pipeIn`), and one for the Script to send the resulting HTML back to the Server (`pipeOut`).
2.  **The Environment:** We translate HTTP headers into CGI environment variables as defined by RFC 3875 (e.g., `HTTP_USER_AGENT`, `QUERY_STRING`, `PATH_INFO`).
3.  **The Fork:** We call `fork()`. Our server splits into two identical processes.
    *   **The Parent (Server):** Takes the ends of the pipes, sets them to non-blocking, adds them to the `poll()` loop, and goes back to managing other clients.
    *   **The Child (Script):** Uses `dup2()` to wire its Standard Input and Standard Output directly into the pipes. It then calls `execve()` to completely replace itself with the Python or PHP interpreter.

This ensures our web server never freezes while a slow script is running!

---

## 7. Phase 6: The Reply (`success.cpp`, `utils.cpp`)

When the work is done (a file was read, or a CGI script finished), we must format the data according to the HTTP standard.

A valid HTTP response looks like this:
```http
HTTP/1.1 200 OK
Content-Type: text/html
Content-Length: 125
Connection: keep-alive

<html>...<body>...</body></html>
```

Whether it's `buildSuccess`, `buildError`, or `processCgiResponse`, our server carefully pieces together the Status Line, the Headers, and the Body into the `Connection::outBuffer_`.

Once the buffer is loaded, the event loop notices `POLLOUT` is ready for that client, and efficiently writes the data out to the network socket in chunks until it is completely sent.

---

## Summary

By combining **Configuration State Machines**, **Non-blocking Sockets**, **I/O Multiplexing (`poll`)**, **Protocol Parsing**, and **Multi-Processing (`fork/execve`)**, this codebase achieves the same foundational architecture used by industry-standard servers like Nginx and Node.js. 

It handles thousands of simultaneous connections without ever using a thread pool, relying entirely on the efficiency of the Operating System's event notification system.
