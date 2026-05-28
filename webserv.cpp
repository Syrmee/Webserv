#include "Config.hpp"
#include "Request.hpp"
#include "Connection.hpp"
#include "Server.hpp"
#include "utils.hpp"
#include "CgiHandler.hpp"

#include <netinet/in.h>
#include <sys/socket.h> 
#include <signal.h>
#include <poll.h>
#include <map>
#include <vector>
#include <cstdlib>
#include <iostream>

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int){ g_running = 0; }




// Helper to add a file descriptor to the pollfd vector
void addToPoll(std::vector<struct pollfd>& fds, int fd, short events) {
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    fds.push_back(pfd);
}

/**
 *  Safely closes a client connection and marks its FD for removal.
 *
 * This function closes the file descriptor, deletes the associated Connection
 * object, and adds the FD to a list of descriptors to be removed from the main
 * poll vector. This "deferred deletion" pattern prevents iterator invalidation
 * within the main event loop.
 */
void closeClient(int fd, std::map<int, Connection*>& conns, 
                 std::map<int, const ServerConfig*>& cfg_map,
                 std::vector<int>& fds_to_remove) 
{
    close(fd);
    
    // SAFETY: Check if it exists before deleting
    if (conns.find(fd) != conns.end())
    {
        delete conns[fd]; // Calls ~Connection()
        conns.erase(fd);
    }
    
    cfg_map.erase(fd);
    fds_to_remove.push_back(fd);
}

/**
 *  Finds the best matching location block for a given request URI.
 *
 * The "best" match is defined as the location with the longest URI prefix that
 * matches the start of the request URI. For example, for a request to "/api/users",
 * a location block for "/api" would be a better match than a location for "/".
 * return A pointer to the best matching Location, or NULL if none found.
 */
const Location* findBestLocation(const ServerConfig* srvCfg, const std::string& uri)
{
    const Location* bestMatch = NULL;
    size_t bestLength = 0;

    for (size_t i = 0; i < srvCfg->locations.size(); ++i)
    {
        const Location& loc = srvCfg->locations[i];
        
        // Check if the URI starts with this location's path (Prefix Match)
        if (uri.compare(0, loc.uri.length(), loc.uri) == 0)
        {    
            // We found a match. Is it better  than the previous one?
            if (loc.uri.length() > bestLength)
            {
                bestMatch = &loc;
                bestLength = loc.uri.length();
            }
        }
    }
    return bestMatch;
}

/**
 *  Processes the raw output of a CGI script into a valid HTTP response.
 *
 * CGI scripts do not return a full HTTP response. Instead, they can provide
 * headers and a body. This function parses that output.
 * 1. It separates the script's output into headers and a body.
 * 2. It specifically looks for a "Status:" header (e.g., "Status: 404 Not Found")
 *    which it uses to generate the HTTP status line. If not found, it defaults to 200 OK.
 * 3. It constructs a proper HTTP response, adding standard server headers and
 *    calculating the `Content-Length` of the body provided by the script.
 *
 * rawOutput The complete string read from the CGI script's stdout.
 * A string containing the full, valid HTTP response.
 */
std::string processCgiResponse(const std::string& rawOutput, const Request& req) {
    std::string headers;
    std::string body;
    
    std::string statusLine = "HTTP/1.1 200 OK\r\n"; 
    int statusInt = 200; // <--- NEW: Default integer status
    
    // 1. Separate Headers from Body
    size_t headerEnd = rawOutput.find("\r\n\r\n");
    if (headerEnd != std::string::npos) {
        headers = rawOutput.substr(0, headerEnd + 2); 
        body = rawOutput.substr(headerEnd + 4);       
    } else {
        body = rawOutput;
    }

    // 2. Check for the "Status:" Header
    size_t statusPos = headers.find("Status:");
    if (statusPos != std::string::npos) {
        size_t endOfLine = headers.find("\r\n", statusPos);
        if (endOfLine != std::string::npos) {
            std::string statusCodeStr = headers.substr(statusPos + 7, endOfLine - (statusPos + 7));
            statusCodeStr = trim(statusCodeStr); // e.g., "404 Not Found"
            
            statusInt = std::atoi(statusCodeStr.c_str()); 
            
            statusLine = "HTTP/1.1 " + statusCodeStr + "\r\n";
            headers.erase(statusPos, endOfLine - statusPos + 2); 
        }
    }

    // 3. Build the Final Response
    std::ostringstream ss;
    ss << statusLine;
    ss << "Server: webserv/1.0\r\n";
    
    if (shouldKeepAlive(req, statusInt))
        ss << "Connection: keep-alive\r\n";
    else
        ss << "Connection: close\r\n";
    
    ss << "Content-Length: " << body.size() << "\r\n";
    ss << headers;

    if (headers.find("Content-Type:") == std::string::npos)
        ss << "Content-Type: text/html\r\n";
    if (!headers.empty() && headers.compare(headers.size() - 2, 2, "\r\n") == 0)
        ss << "\r\n"; 
    else
        ss << "\r\n\r\n"; 

    ss << body;
    return ss.str();
}

/**
 * Handles all logic for an existing client connection based on poll events.
 *
 * This is the primary request-handling function. It is called when an existing
 * client connection has data to read or is ready to receive data.
 *
 * If the event is POLLIN (data available to read):
 * 1. Reads data from the socket into the connection's buffer.
 * 2. If headers are complete, it parses them, including the body (handling both
 *    `Content-Length` and chunked encoding).
 * 3. Performs routing by finding the best location block.
 * 4. Validates the request against the location's rules (e.g., allowed methods).
 * 5. Dispatches the request to the appropriate handler: CGI, static file serving,
 *    directory listing, file upload (POST), or file deletion (DELETE).
 * 6. The handler generates a response and places it in the connection's output buffer.
 *
 * If the event is POLLOUT (socket ready to write):
 * 1. Writes data from the output buffer to the socket.
 */
bool handleClient(Connection* conn, const ServerConfig* srvCfg, short revents, short& poll_events,
                  std::vector<struct pollfd>& fds,
                  std::map<int, Connection*>& cgi_read_map,
                  std::map<int, Connection*>& cgi_write_map)
{
    // 1. Handle critical socket errors immediately
    if (revents & (POLLERR | POLLNVAL))
        return false;

    // Only close on POLLHUP if we have nothing left to send or read
    if ((revents & POLLHUP) && !(revents & POLLIN) && conn->out().empty())
        return false;

    // 2. READ (Client sent data)
    if (revents & (POLLIN | POLLHUP))
    {
        try {
            // Read available data. We don't return on 0 here because 
            // we must process any data already in the buffer first.
            int bytesRead = conn->readFromSocket();
            // If client closed (EOF) and we have no data to process and nothing to send, close.
            if (bytesRead == 0 && conn->in().empty() && conn->out().empty())
                return false;
        } catch (const std::exception& e) {
            return false;
        }
    }

    // 2.5. PROCESS (Loop to handle Pipelined requests)
    size_t headerEnd;
    while (conn->out().empty() && (headerEnd = conn->in().find("\r\n\r\n")) != std::string::npos)
    {
        try 
        {
            // Check for end of headers
            size_t headerEnd = conn->in().find("\r\n\r\n");
            if (headerEnd != std::string::npos)
            {
                // A. Parse Headers
                conn->request().reset();
                conn->request().parseHeader(conn->in().c_str());

                // B. Check Config Limits 
                size_t effective_max_body_size = srvCfg->client_max_body_size;
                const Location* loc = findBestLocation(srvCfg, conn->request().getPath());
                if (loc && loc->client_max_body_size != 0)
                    effective_max_body_size = loc->client_max_body_size;

                size_t contentLength = conn->request().getContentLength();
                if (contentLength > effective_max_body_size)
                    throw Request::ParseError(413);

                // C. Check for Full Body
                size_t headerSize = headerEnd + 4;
                std::string finalBody;
                size_t bytesToConsume = 0;

                if (conn->request().isChunked())
                {
                    // 1. isolate body from the socket buffer
                    std::string rawBody = conn->in().substr(headerSize);
                    // 2. feed it to unchunker
                    std::string decoded;
                    bool isDone = conn->request().parseChunkedBody(rawBody, decoded);
                    conn->request().getUnchunkedBody() += decoded;
                    // 3. left overs (the next pipelined request) goes back to socket buffer
                    std::string head = conn->in().substr(0, headerSize);
                    conn->in() = head + rawBody;
                    if (!isDone)
                        break; 
                    // 4. finnal body
                    finalBody = conn->request().getUnchunkedBody();
                    bytesToConsume = headerSize;
                }
                else
                {
                    bytesToConsume = headerSize + contentLength;
                    size_t totalRequestSize = headerSize + contentLength;
                    if (conn->in().size() < totalRequestSize)
                        break;
                    
                    finalBody = conn->in().substr(headerSize, contentLength);
                    conn->request().setUnchunkedBody(finalBody);
                }
                // =============================================================
                // D. ROUTING LOGIC
                // =============================================================
                
                // 1. Find Best Location
                if (!loc)
                    throw HttpError(404);

                // 2. Check HTTP Method
                bool methodAllowed = false;
                if (loc->methods.empty())
                    methodAllowed = (conn->request().getMethod() == "GET");
                else
                {
                    for (size_t k = 0; k < loc->methods.size(); ++k)
                    {
                        if (loc->methods[k] == conn->request().getMethod())
                        {
                            methodAllowed = true;
                            break;
                        }
                    }
                }
                if (!methodAllowed)
                {
                    // Construct the "Allow" header string organically
                    std::string allowed = "";
                    if (loc->methods.empty()) allowed = "GET";
                    else {
                        for (size_t k = 0; k < loc->methods.size(); ++k) {
                            allowed += loc->methods[k];
                            if (k + 1 < loc->methods.size()) allowed += ", ";
                        }
                    }
                    throw HttpError(405, allowed); // Pass it via the exception
                }

                // 3. Handle Redirection
                if (!loc->return_url.empty())
                    throw HttpError(loc->return_code, loc->return_url);
                
                // =============================================================
                // 4. PATH RESOLUTION & CGI HANDLING
                // =============================================================
                std::string fsPath;
                bool cgiRequestHandled = false;

                if (!loc->cgi_extension.empty()) {
                    std::string requestPath = conn->request().getPath();
                    std::string pathInfo;
                    std::string scriptName;

                    // Walk backwards up the request URI to find the script file
                    for (size_t split_pos = requestPath.length(); split_pos != std::string::npos; split_pos = (split_pos > 0) ? requestPath.rfind('/', split_pos - 1) : std::string::npos)
                    {
                        scriptName = requestPath.substr(0, split_pos);
                        if (scriptName.empty() && requestPath[0] == '/') scriptName = "/"; // Handle root case
                        
                        try { fsPath = resolveFullPath(*loc, scriptName); }
                        catch (const HttpError&) { fsPath.clear(); }

                        if (!fsPath.empty() && isFile(fsPath) && fsPath.length() >= loc->cgi_extension.length() &&
                            fsPath.substr(fsPath.length() - loc->cgi_extension.length()) == loc->cgi_extension)
                        {
                            // Found it! The rest of the path is PATH_INFO.
                            if (split_pos < requestPath.length())
                                pathInfo = requestPath.substr(split_pos);

                            cgiRequestHandled = true;
                            break;
                        }
                    }

                    if (cgiRequestHandled) {
                        if (access(fsPath.c_str(), R_OK) != 0) throw HttpError(403);

                        // 1. Create and execute the CGI process
                        conn->setCgiHandler(new CgiHandler(conn->request(), *loc, fsPath, scriptName, pathInfo, loc->cgi_path));
                        conn->getCgiHandler()->executeCgi();

                        // 2. Get the new pipe File Descriptors
                        int readFd = conn->getCgiHandler()->getReadFd();
                        int writeFd = conn->getCgiHandler()->getWriteFd();


                        addToPoll(fds, readFd, POLLIN);
                        addToPoll(fds, writeFd, POLLOUT);


                        cgi_read_map[readFd] = conn;
                        cgi_write_map[writeFd] = conn;


                        conn->in().erase(0, bytesToConsume);
                        
                        return true; 
                    }
                }

                // If not a CGI request, or if script lookup failed, do normal path resolution.
                fsPath = resolveFullPath(*loc, conn->request().getPath());

                // 5. Handle standard methods (GET, POST, DELETE)
                if (conn->request().getMethod() == "GET" || conn->request().getMethod() == "HEAD")
                {
                    // 5. Handle Directory
                    if (isDir(fsPath)) 
                    {
                        // 1. Redirect if trailing slash is missing (e.g. "/images" -> "/images/")
                        // This is required so relative links in HTML work correctly.
                        std::string path = conn->request().getPath();
                        if (path.empty() || path[path.length() - 1] != '/')
                        {
                            std::string newLoc = conn->request().getPath() + "/";
                            if (!conn->request().getQuery().empty()) 
                                newLoc += "?" + conn->request().getQuery();
                            throw HttpError(301, newLoc);
                        }

                        // 2. Try Index File
                        if (!loc->index.empty())
                        {
                            std::string indexPath = fsPath + (fsPath[fsPath.length() - 1] == '/' ? "" : "/") + loc->index;
                            if (isFile(indexPath))
                            {
                                fsPath = indexPath; 
                                goto serve_file; 
                            }
                        }

                        // 3. Try Autoindex
                        if (loc->autoindex)
                        {
                            conn->out() = buildDirectoryListing(conn->request(), fsPath);
                            conn->in().erase(0, bytesToConsume);

                            poll_events = POLLIN | POLLOUT;
                            return true; 
                        }

                        // 4. If neither worked -> Forbidden
                        throw HttpError(403);
                    }
                    else if (!isFile(fsPath))
                    {
                        throw HttpError(404);
                    }

                    serve_file:
                    // E. Build Response
                    conn->out() = buildSuccess(conn->request(), fsPath, srvCfg);
                }
                else if (conn->request().getMethod() == "DELETE")
                {
                    // 1. check if it exists
                    if (!isFile(fsPath))
                        throw HttpError(404);
                    // 2. check permissions (write access needed to delete)
                    if (access(fsPath.c_str(), W_OK) != 0)
                        throw HttpError(403);
                    // 3. delete
                    if (std::remove(fsPath.c_str()) != 0)
                        throw HttpError(500); // interal error code
                    // 4. send 204 No content
                    bool keepAlive = shouldKeepAlive(conn->request(), 204);
                    std::ostringstream response;

                    response    <<  "HTTP/1.1 204 No Content\r\n"
                                <<  "Server: webserv/0.1\r\n"
                                <<  "Date: " << httpDateNow() << "\r\n"
                                <<  "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n"
                                <<  "\r\n";
                    conn->out() = response.str();  
                }
                else if (conn->request().getMethod() == "POST")
                {
                    // 1. Check if upload store is configured
                    if (loc->upload_store.empty())
                    {
                        std::cerr << "Error: upload store not set for this location" << std::endl;
                        throw HttpError(500);
                    }

                    // 2. Determine filename from URI
                    // Example: POST /uploads/cat.png -> filename = "cat.png"
                    std::string uri = conn->request().getPath();
                    std::string filename = uri.substr(uri.find_last_of('/') + 1);

                    if (filename.empty())
                        throw HttpError(400); // cannot upload to a directory root
                    
                    // 3. construct full path
                    std::string outPath = loc->upload_store;
                    // ensure traling slash on directory
                    if (!outPath.empty() && outPath[outPath.size() - 1] != '/')
                        outPath += "/";
                    outPath += filename;

                    // 4. write to file (binary mode is for images)
                    std::ofstream outFile(outPath.c_str(), std::ios::out | std::ios::binary);
                    if (!outFile.is_open())
                    {
                        std::cerr << "Error: could not open " << outPath << " for writing" << std::endl;
                        throw HttpError(500);
                    }
                    // write the body data
                    outFile.write(finalBody.c_str(), finalBody.size());
                    outFile.close();

                    if (outFile.fail())
                        throw HttpError(500);

                    // 5. send 201 Created response
                    bool keepAlive = shouldKeepAlive(conn->request(), 201);

                    std::ostringstream response;
                    response << "HTTP/1.1 201 Created\r\n"
                             << "Server: webserv/0.1\r\n"
                             << "Date: " << httpDateNow() << "\r\n"
                             << "Content-Length: 0\r\n"
                             << "Location: " << conn->request().getPath() << "\r\n"
                             << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n"
                             << "\r\n";

                    conn->out() = response.str();
                }
                // F. Consume Input
                conn->in().erase(0, bytesToConsume);
            }
        }
        catch (const Request::ParseError& e)
        {
            conn->out() = buildError(e.status(), conn->request(), srvCfg);
            conn->in().clear();
            break;
        }
        catch (const HttpError& he)
        {
            int status = he.status();
            // Calculate how many bytes the current request occupied
            size_t headPos = conn->in().find("\r\n\r\n");
            if (headPos != std::string::npos) {
                size_t toRemove = headPos + 4 + conn->request().getContentLength();
                // Only erase the current request, preserving any 'pipelined' data
                if (toRemove > conn->in().size() || conn->request().isChunked()) toRemove = conn->in().size();
                conn->in().erase(0, toRemove);
            } else {
                conn->in().clear();
            }

            if (status >= 300 && status < 400)
                conn->out() = buildRedirect(conn->request(), status, he.location());
            else if (status == 405)
                conn->out() = buildError(status, conn->request(), srvCfg, he.location());
            else
                conn->out() = buildError(status, conn->request(), srvCfg);
            break;
        }
        catch (const std::exception& e)
        {
            std::cerr << "Request Error: " << e.what() << std::endl;
            conn->out() = buildError(500, conn->request(), srvCfg);
            conn->in().clear();
            break;
        }
    }

    // 3. WRITE (Ready to send response)
    if (revents & POLLOUT)
    {
        int currentStatus = 200;
        if (conn->out().find(" 405 ") != std::string::npos) currentStatus = 405;
        else if (conn->out().find(" 413 ") != std::string::npos) currentStatus = 413;
        else if (conn->out().find(" 400 ") != std::string::npos) currentStatus = 400;
        else if (conn->out().find(" 403 ") != std::string::npos) currentStatus = 403;
        else if (conn->out().find(" 404 ") != std::string::npos) currentStatus = 404;
        else if (conn->out().find(" 500 ") != std::string::npos) currentStatus = 500;
        else if (conn->out().find(" 400 ") != std::string::npos) currentStatus = 400;
        else if (conn->out().find(" 500 ") != std::string::npos) currentStatus = 500;
        else if (conn->out().find(" 301 ") != std::string::npos) currentStatus = 301;
        else if (conn->out().find(" 201 ") != std::string::npos) currentStatus = 201;
        else if (conn->out().find(" 204 ") != std::string::npos) currentStatus = 204;

        bool keep = shouldKeepAlive(conn->request(), currentStatus);

        int bytes = conn->writeToSocket();
        if (bytes < 0)
            return false;

        if (conn->out().empty())
        {
            if (!keep)
                return false;
        }
    }

    // Final check: if the socket is half-closed (read 0 previously) 
    // and we've finished sending everything, close now.
    if ((revents & POLLHUP) && conn->out().empty())
        return false;

    // 4. Update Poll Flags
    if (!conn->out().empty())
        poll_events = POLLIN | POLLOUT;
    else
        poll_events = POLLIN;

    return true;
}

/**
 *  The main function and entry point for the webserver.
 *
 * The `main` function orchestrates the entire server lifecycle:
 * 1. Parses the configuration file provided as a command-line argument.
 * 2. Sets up server sockets for each configured host:port, binding and listening on them.
 * 3. Initializes the `poll()` structure with the listening sockets.
 * 4. Enters the main event loop, which uses `poll()` to wait for I/O events on all
 *    file descriptors (listeners, clients, and CGI pipes).
 * 5. Dispatches events to the appropriate handlers until a SIGINT is received.
 */

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::cerr << "Usage: ./webserv [config_file]" << std::endl;
        return 1;
    }

    signal(SIGINT, on_sigint);

    try {
        // configuration setup
        Config config;
        config.parse(argv[1]);

        // ====================================================================
        // 2. SERVER SETUP PHASE
        // ====================================================================
        std::vector<Server*> servers;
        std::vector<struct pollfd> poll_fds;

        // Map to quickly identify if an FD belongs to a Listening Server
        // Key: FD, Value: Pointer to Server object
        std::map<int, Server*> listener_map;

        // Loop 1: Create and Bind
        for (size_t i = 0; i < config.servers.size(); ++i)
        {
            Server* srv = new Server(config.servers[i]);
            
            if (!srv->bindAndListen())
            {
                std::cerr << "Error: Failed to bind port " << config.servers[i].port 
                          << ". Skipping." << std::endl;
                delete srv;
                continue; 
            }
            servers.push_back(srv);
        }

        if (servers.empty())
        {
            std::cerr << "No server" << std::endl;
            return 1;
        }

        // Initialize Poll and Listener Map
        for (size_t i = 0; i < servers.size(); ++i)
        {
            addToPoll(poll_fds, servers[i]->getFd(), POLLIN);
            listener_map[servers[i]->getFd()] = servers[i];
            
            std::cout << "Server listening on " << servers[i]->getConfig().host 
                      << ":" << servers[i]->getConfig().port << std::endl;
        }

        // ====================================================================
        // 3. MAIN EVENT LOOP
        // ====================================================================
        // Key: Client FD, Value: Connection Object (POINTER)
        std::map<int, Connection*> connections;
        // Allows us to know which Config rules apply to a specific Client FD
        std::map<int, const ServerConfig*> client_to_config;

        // [NEW] CGI Maps
        std::map<int, Connection*> cgi_read_map;
        std::map<int, Connection*> cgi_write_map;

        std::cout << "Server is running..." << std::endl;

        while (g_running)
        {
            std::vector<int> fds_to_remove; // Collect FDs to be removed after loop

            // A. Wait for events (1000ms timeout)
            int ret = poll(&poll_fds[0], poll_fds.size(), 1000);
            
            if (ret < 0)
            {
                if (errno == EINTR)
                    continue; // Signal received, just loop back
                perror("poll");
                break; // Real error, exit
            }
            if (ret == 0)
                continue; // Timeout, loop back

            // B. Iterate through events
            
            for (size_t i = 0; i < poll_fds.size(); ++i)
            {
                int fd = poll_fds[i].fd;
                short revents = poll_fds[i].revents;

                if (revents == 0)
                    continue;

                // ---------------------------------------------------------
                // CASE 1: NEW CONNECTION (Listener)
                // ---------------------------------------------------------
                if (listener_map.count(fd))
                {
                    if (revents & POLLIN)
                    {
                        int client_fd = listener_map[fd]->acceptClient();
                        if (client_fd >= 0)
                        {
                            // Create new connection object (Allocate on HEAP)
                            Connection* new_conn = new Connection(client_fd);
                            connections[client_fd] = new_conn;
                            
                            // Map the client to the server config that accepted it
                            client_to_config[client_fd] = &listener_map[fd]->getConfig();

                            // Only listen for POLLIN initially!
                            addToPoll(poll_fds, client_fd, POLLIN);
                            
                            std::cout << "New Client Connected: " << client_fd << std::endl;
                        }
                    }
                }

                // ---------------------------------------------------------
                // CASE 2: CGI SCRIPT HAS OUTPUT TO READ
                // ---------------------------------------------------------
                else if (cgi_read_map.count(fd))
                {
                    Connection *conn = cgi_read_map[fd];
                    if ((revents & POLLIN) || (revents & POLLHUP))
                    {
                        char buffer[4096];
                        ssize_t bytes = read(fd, buffer, sizeof(buffer) - 1);

                        if (bytes > 0){
                            conn->getCgiOutput().append(buffer, bytes);}
                        else if (bytes == 0) // EOF
                        {
                            int status;
                            waitpid(conn->getCgiHandler()->getPid(), &status, 0);
                            conn->getCgiHandler()->clearPid();
                            conn->out() = processCgiResponse(conn->getCgiOutput(), conn->request());
                            delete conn->getCgiHandler();
                            conn->setCgiHandler(NULL);
                            // 4. close pipe
                            close(fd);
                            cgi_read_map.erase(fd);
                            fds_to_remove.push_back(fd);
                            // 5. switch client back to write mode
                            for (size_t k = 0; k < poll_fds.size(); ++k)
                            {
                                if (poll_fds[k].fd == conn->fd())
                                {
                                    poll_fds[k].events = POLLIN | POLLOUT;
                                    break;
                                }
                            }
                        }
                    }
                    else if (revents & (POLLERR | POLLHUP))
                    {
                        // Handle crash/hangup
                        close(fd);
                        cgi_read_map.erase(fd);
                        fds_to_remove.push_back(fd);
                    }
                }

                // ---------------------------------------------------------
                // CASE 3: CGI SCRIPT IS READY TO RECEIVE POST DATA
                // ---------------------------------------------------------
                else if (cgi_write_map.count(fd))
                {
                    if (revents & POLLOUT)
                    {
                        Connection *conn = cgi_write_map[fd];

                        // 1. Get the data
                        const std::string& body = conn->request().getUnchunkedBody();
                        size_t bytesToSend = body.size();
                        size_t bytesAlreadySent = conn->getCgiBytesWritten();

                        // 2, write whats left
                        size_t remaining = bytesToSend - bytesAlreadySent;
                        if (remaining > 0)
                        {
                            size_t written = write(fd, body.c_str() + bytesAlreadySent, remaining);
                            if (written > 0)
                                conn->addCgiBytesWritten(written);
                        }
                        // 3. Close if done
                        if (conn->getCgiBytesWritten() >= bytesToSend)
                        {
                            close(fd);
                            cgi_write_map.erase(fd);
                            fds_to_remove.push_back(fd);
                        }
                    }
                    // handle Errors
                    else if (revents & (POLLERR | POLLHUP))
                    {
                        close(fd);
                        cgi_write_map.erase(fd);
                        fds_to_remove.push_back(fd);
                    }
                }

                // ---------------------------------------------------------
                // CASE 4: EXISTING CLIENT
                // ---------------------------------------------------------
                else
                {
                    // If FD is not in connections map, it's a ghost. Safety check.
                    if (connections.find(fd) == connections.end())
                    {
                        closeClient(fd, connections, client_to_config, fds_to_remove);
                        continue;
                    }

                    // Delegate all logic to the handler
                    // We pass the poll_events by reference so the handler can toggle POLLOUT
                    bool keepAlive = handleClient(
                        connections[fd], 
                        client_to_config[fd], 
                        revents, 
                        poll_fds[i].events,
                        poll_fds,
                        cgi_read_map,
                        cgi_write_map
                    );

                    if (!keepAlive)
                        closeClient(fd, connections, client_to_config, fds_to_remove);
                }
            }

            // Clean up closed FDs from poll_fds outside the main loop to prevent iterator invalidation
            for (size_t k = 0; k < fds_to_remove.size(); ++k) {
                for (std::vector<struct pollfd>::iterator it = poll_fds.begin(); it != poll_fds.end(); ) {
                    if (it->fd == fds_to_remove[k]) {
                        it = poll_fds.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
        }
        
        // CLEANUP AT EXIT
        for (std::map<int, Connection*>::iterator it = connections.begin(); it != connections.end(); ++it)
        {
            close(it->first);
            delete it->second;
        }
        for (size_t i = 0; i < servers.size(); ++i)
        {
            delete servers[i];
        }

    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\nServer shutting down..." << std::endl;
    return 0;
}