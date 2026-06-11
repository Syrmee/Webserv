*This project has been created as part of the 42 curriculum by ggochita, alrey, eperrier.*

## Description
Webserv is a custom HTTP web server written in C++98. The main goal of this project is to understand how a web server operates behind the scenes by building one from scratch. We designed a non-blocking, event-driven architecture using the poll system call. This allows the server to efficiently handle multiple client connections simultaneously on a single thread without freezing.

Our web server can parse configuration files, route incoming requests, serve static files, generate directory listings, handle file uploads, and execute CGI scripts. By combining socket programming and I/O multiplexing, it mimics the core concepts found in modern industry-standard servers.

## Instructions
To compile the server, you need a C++ compiler and make. Run the following command in the root directory:

    make

After compilation, you can start the server by providing it with a configuration file:

    ./webserv your_config.conf

To gracefully shut down the server, press Ctrl+C.

## Resources
During the development of Webserv, we relied on several classic technical references:
* RFC 7230 and RFC 7231 for understanding HTTP/1.1 message syntax and routing semantics.
* RFC 3875 for the Common Gateway Interface (CGI) specification.
* Beej's Guide to Network Programming for socket programming fundamentals.
* Linux manual pages for detailed documentation on system calls like socket, bind, listen, accept, poll, and fcntl.

### AI Usage
We used AI to understand single concepts well and to handle repetitive tasks. It also served as one of our main resources during research, helping us clarify complex API behaviors and HTTP specifications.