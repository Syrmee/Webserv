#include "Config.hpp"
#include "Request.hpp"
#include "Connection.hpp"
#include "Server.hpp"
#include "utils.hpp"
#include "CgiHandler.hpp"

#include <netinet/in.h>
#include <sys/socket.h> 
#include <sys/wait.h>
#include <signal.h>
#include <poll.h>
#include <map>
#include <vector>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <fcntl.h>

static volatile sig_atomic_t g_running = 1;
static void on_sigint(int){ g_running = 0; }




// Helper to add a file descriptor to the pollfd vector
void addToPoll(std::vector<struct pollfd>& fds, int fd, short events) {
    struct pollfd pfd = {};
    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;
    fds.push_back(pfd);
}

bool isFdScheduledForRemoval(const std::vector<int>& fds_to_remove, int fd)
{
    for (size_t i = 0; i < fds_to_remove.size(); ++i)
    {
        if (fds_to_remove[i] == fd)
            return true;
    }
    return false;
}

void closeCgiReadPipe(CgiHandler* cgi,
                      std::map<int, Connection*>& cgi_read_map,
                      std::vector<int>& fds_to_remove)
{
    if (!cgi)
        return;

    int fd = cgi->getReadFd();
    if (fd < 0)
        return;

    cgi->closeReadFd();
    cgi_read_map.erase(fd);
    fds_to_remove.push_back(fd);
}

void closeCgiWritePipe(CgiHandler* cgi,
                       std::map<int, Connection*>& cgi_write_map,
                       std::vector<int>& fds_to_remove)
{
    if (!cgi)
        return;

    int fd = cgi->getWriteFd();
    if (fd < 0)
        return;

    cgi->closeWriteFd();
    cgi_write_map.erase(fd);
    fds_to_remove.push_back(fd);
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
                 std::vector<int>& fds_to_remove,
                 std::map<int, Connection*>& cgi_read_map,
                 std::map<int, Connection*>& cgi_write_map) 
{
    
    // SAFETY: Check if it exists before deleting
    if (conns.find(fd) != conns.end())
    {
        Connection* conn = conns[fd];
        if (conn->getCgiHandler())
        {
            closeCgiReadPipe(conn->getCgiHandler(), cgi_read_map, fds_to_remove);
            closeCgiWritePipe(conn->getCgiHandler(), cgi_write_map, fds_to_remove);
        }
        conn->closeNow();
        delete conn;
        conns.erase(fd);
    }
    else
    {
        ::close(fd);
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
    // If CGI produced absolutely no output, it's an invalid response (502)
    if (rawOutput.empty()) {
        return ""; 
    }

    size_t first_newline = rawOutput.find('\n');
    size_t first_colon = rawOutput.find(':');

    size_t headerEnd = std::string::npos;
    size_t separatorLength = 0;

    if (first_colon != std::string::npos && (first_newline == std::string::npos || first_colon < first_newline)) {
        size_t rnrn = rawOutput.find("\r\n\r\n");
        size_t nn = rawOutput.find("\n\n");
        
        if (rnrn != std::string::npos && nn != std::string::npos) {
            if (rnrn < nn) { headerEnd = rnrn; separatorLength = 4; }
            else           { headerEnd = nn;   separatorLength = 2; }
        } else if (rnrn != std::string::npos) {
            headerEnd = rnrn; separatorLength = 4;
        } else if (nn != std::string::npos) {
            headerEnd = nn;   separatorLength = 2;
        }
    }

    std::string rawHeaders;
    size_t bodyStart = 0;
    size_t bodySize = rawOutput.size();

    if (headerEnd != std::string::npos) {
        rawHeaders = rawOutput.substr(0, headerEnd);
        bodyStart = headerEnd + separatorLength;
        bodySize = rawOutput.size() - bodyStart;
    }

    // 2. Parse CGI Headers Line by Line
    std::vector<std::string> cleanHeaders;
    std::string statusLine = "HTTP/1.1 200 OK\r\n";
    int statusInt = 200;
    bool hasContentType = false;

    if (!rawHeaders.empty()) {
        std::istringstream stream(rawHeaders);
        std::string line;
        while (std::getline(stream, line)) {
            if (!line.empty() && line[line.size() - 1] == '\r') {
                line.erase(line.size() - 1);
            }
            if (line.empty()) continue;

            std::string lower_line = toLower(line);

            if (lower_line.find("status:") == 0) {
                std::string statusCodeStr = line.substr(7);
                statusCodeStr = trim(statusCodeStr);
                statusInt = std::atoi(statusCodeStr.c_str());
                statusLine = "HTTP/1.1 " + statusCodeStr + "\r\n";
            } 
            else if (lower_line.find("connection:") == 0 ||
                     lower_line.find("content-length:") == 0 ||
                     lower_line.find("server:") == 0 ||
                     lower_line.find("date:") == 0)
            {
                continue;
            }
            else if (lower_line.find("http/1.") == 0) {
                continue; 
            }
            else {
                if (lower_line.find("content-type:") == 0) {
                    hasContentType = true;
                }
                cleanHeaders.push_back(line + "\r\n");
            }
        }
    }

    // 3. Build the Final Clean HTTP Response
    std::string response;
    response.reserve(1024 + bodySize); 

    response += statusLine;
    response += "Server: webserv/1.0\r\n";

    if (shouldKeepAlive(req, statusInt))
        response += "Connection: keep-alive\r\n";
    else
        response += "Connection: close\r\n";
    
    std::ostringstream cl;
    cl << bodySize;
    response += "Content-Length: " + cl.str() + "\r\n";

    for (size_t i = 0; i < cleanHeaders.size(); ++i) {
        response += cleanHeaders[i];
    }

    if (!hasContentType) {
        response += "Content-Type: text/html\r\n";
    }

    response += "\r\n";
    response.append(rawOutput, bodyStart, bodySize);
    return response;
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
    if ((revents & POLLHUP) && !(revents & POLLIN) && conn->out().empty() && !conn->getCgiHandler())
        return false;

    // A. READ PHASE
    if (revents & (POLLIN | POLLHUP))
    {
        try {
            int bytesRead = conn->readFromSocket();
            if (bytesRead == -1)
                return false;
            if (bytesRead == 0 && conn->in().empty() && conn->out().empty() && !conn->getCgiHandler())
                return false;
            if (bytesRead > 0)
                conn->touchActivity();
        } catch (const std::exception& e) {
            return false;
        }
    }

    // B. WRITE PHASE
    if (revents & POLLOUT)
    {
        int currentStatus = 200;
        if (conn->out().find(" 405 ") != std::string::npos) currentStatus = 405;
        else if (conn->out().find(" 413 ") != std::string::npos) currentStatus = 413;
        else if (conn->out().find(" 400 ") != std::string::npos) currentStatus = 400;
        else if (conn->out().find(" 403 ") != std::string::npos) currentStatus = 403;
        else if (conn->out().find(" 404 ") != std::string::npos) currentStatus = 404;
        else if (conn->out().find(" 500 ") != std::string::npos) currentStatus = 500;
        else if (conn->out().find(" 301 ") != std::string::npos) currentStatus = 301;
        else if (conn->out().find(" 201 ") != std::string::npos) currentStatus = 201;
        else if (conn->out().find(" 204 ") != std::string::npos) currentStatus = 204;

        bool keep = shouldKeepAlive(conn->request(), currentStatus);
        
        int bytes = conn->writeToSocket();
        if (bytes == -1) return false;
        if (bytes > 0) conn->touchActivity();
        
        if (conn->out().empty() && !keep) return false;
    }

    // C. PROCESSING PHASE (Runs last to drain pipelined/chunked data instantly)
    while (conn->out().empty())
    {
        if (conn->getCgiHandler() != NULL)
            break;

        size_t headerEnd = conn->in().find("\r\n\r\n");
        size_t headerSize = (headerEnd != std::string::npos) ? (headerEnd + 4) : 0;

        try
        {
            if (conn->request().getMethod().empty())
            {
                if (headerEnd == std::string::npos)
                    break;
                std::string headerStr = conn->in().substr(0, headerSize);
                conn->request().parseHeader(headerStr.c_str());
            }
        
        // =============================================================
        // D. ROUTING & PRE-BODY VALIDATION
        // =============================================================
        const Location* loc = findBestLocation(srvCfg, conn->request().getPath());
        if (!loc)
            throw HttpError(404);

        // Check HTTP Method
        bool methodAllowed = false;
        if (loc->methods.empty())
            methodAllowed = (conn->request().getMethod() == "GET");
        else {
            for (size_t k = 0; k < loc->methods.size(); ++k) {
                if (loc->methods[k] == conn->request().getMethod()) {
                    methodAllowed = true;
                    break;
                }
            }
        }
        if (!methodAllowed) {
            std::string allowed = "";
            if (loc->methods.empty()) allowed = "GET";
            else {
                for (size_t k = 0; k < loc->methods.size(); ++k) {
                    allowed += loc->methods[k];
                    if (k + 1 < loc->methods.size()) allowed += ", ";
                }
            }
            throw HttpError(405, allowed);
        }

        // Handle Redirection
        if (!loc->return_url.empty())
            throw HttpError(loc->return_code, loc->return_url);

        if (!loc->cgi_extension.empty() && conn->request().getPath().length() >= loc->cgi_extension.length() && conn->request().getPath().substr(conn->request().getPath().length() - loc->cgi_extension.length()) == loc->cgi_extension) {
            std::string fsPath = resolveFullPath(*loc, conn->request().getPath());
        }

        // B. Check Config Limits

            size_t effective_max_body_size = srvCfg->client_max_body_size;
            if (loc && loc->client_max_body_size != 0)
                effective_max_body_size = loc->client_max_body_size;

            size_t contentLength = conn->request().getContentLength();
            if (contentLength > effective_max_body_size)
                throw Request::ParseError(413);

            size_t bytesToConsume = 0;

            if (conn->request().isChunked())
            {
                if (conn->request().getChunkedRawOffset() == 0)
                    conn->request().setChunkedRawOffset(headerSize);

                bool isDone = conn->request().parseChunkedBody(conn->in());

                // Vérification de la taille à la volée
                if (conn->request().getBodyLen() > effective_max_body_size)
                {
                    throw Request::ParseError(413); // Payload Too Large !
                }

                if (!isDone)
                    break;
                bytesToConsume = conn->request().getChunkedRawOffset();
            }
            else
            {
                bytesToConsume = headerSize + contentLength;
                if (conn->in().size() < bytesToConsume)
                    break;

                conn->request().setBodyView(conn->in().c_str() + headerSize, contentLength);
            }
                // =============================================================
                // 4. PATH RESOLUTION & CGI HANDLING
                // =============================================================      
            std::string fsPath;

            if (!loc->cgi_extension.empty()) {
                const std::string& requestPath = conn->request().getPath();
                if (requestPath.length() >= loc->cgi_extension.length() &&
                    requestPath.substr(requestPath.length() - loc->cgi_extension.length()) == loc->cgi_extension)
                {
                    fsPath = resolveFullPath(*loc, requestPath);
                    std::string scriptName = requestPath;
                    std::string pathInfo = "";

                    // 1. Create and execute the CGI process
                    conn->setCgiHandler(new CgiHandler(conn->request(), *loc, fsPath, scriptName, pathInfo, loc->cgi_path));
                    conn->getCgiHandler()->executeCgi();
                    conn->setCgiStartTime(time(NULL));

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
                    // 1. Try Index File
                    if (!loc->index.empty())
                    {
                        std::string indexPath = fsPath + (fsPath[fsPath.length() - 1] == '/' ? "" : "/") + loc->index;
                        if (isFile(indexPath))
                        {
                            fsPath = indexPath;
                            goto serve_file;
                        }
                    }

                    // 2. Redirect if trailing slash is missing
                    std::string path = conn->request().getPath();
                    if (path.empty() || path[path.length() - 1] != '/')
                    {
                        std::string newLoc = conn->request().getPath() + "/";
                        if (!conn->request().getQuery().empty())
                            newLoc += "?" + conn->request().getQuery();
                        throw HttpError(301, newLoc);
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
                    throw HttpError(404);
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
                    throw HttpError(500);
                }
                // write the body data
                const char* bodyPtr = conn->request().getBodyPtr();
                outFile.write(bodyPtr, conn->request().getBodyLen());
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
            // Compact input buffer to release memory if vastly overallocated
            if (conn->in().capacity() > 4096 && conn->in().size() < conn->in().capacity() / 4)
            {
                std::string compacted(conn->in());
                conn->in().swap(compacted);
            }
        }
        catch (const Request::ParseError& e)
        {
            conn->out() = buildError(e.status(), conn->request(), srvCfg);
            std::string().swap(conn->in());
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
                std::string().swap(conn->in());
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
            conn->out() = buildError(500, conn->request(), srvCfg);
            std::string().swap(conn->in());
            break;
        }
    }

    // 3. WRITE (Ready to send response)
    // Final check: if the socket is half-closed (read 0 previously) 
    // and we've finished sending everything, close now.
    if ((revents & POLLHUP) && conn->out().empty() && !conn->getCgiHandler())
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
    signal(SIGPIPE, SIG_IGN); // Protects against broken pipes

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
                delete srv;
                continue;
            }
            servers.push_back(srv);
        }
        if (servers.empty())
        {
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

            time_t now = time(NULL);

            // --- CGI Timeout Sweep ---
            for (std::map<int, Connection*>::iterator it = connections.begin(); it != connections.end(); ++it)
            {
                Connection* conn = it->second;
                if (conn->getCgiHandler() && (now - conn->getCgiStartTime() > 300))
                {
                    std::cerr << "[CGI TIMEOUT] Killing CGI after 300s" << std::endl;
                    if (conn->getCgiHandler()->getPid() > 0) {
                        kill(conn->getCgiHandler()->getPid(), SIGKILL);
                        waitpid(conn->getCgiHandler()->getPid(), NULL, 0);
                        conn->getCgiHandler()->clearPid();
                    }
                    closeCgiReadPipe(conn->getCgiHandler(), cgi_read_map, fds_to_remove);
                    closeCgiWritePipe(conn->getCgiHandler(), cgi_write_map, fds_to_remove);
                    
                    conn->out() = buildError(504, conn->request(), client_to_config[conn->fd()]);
                    conn->touchActivity(); // Prevents idle timeout from killing the 504 response
                    for (size_t k = 0; k < poll_fds.size(); ++k) {
                        if (poll_fds[k].fd == conn->fd()) { poll_fds[k].events = POLLIN | POLLOUT; break; }
                    }
                    delete conn->getCgiHandler(); conn->setCgiHandler(NULL);
                }
            }

            // --- Client Idle Timeout Sweep (60s) ---
            {
                std::vector<int> idle_fds;
                for (std::map<int, Connection*>::iterator it = connections.begin(); it != connections.end(); ++it)
                {
                    Connection* conn = it->second;
                    if (!conn->getCgiHandler() && (now - conn->getLastActivity()) > 60)
                        idle_fds.push_back(it->first);
                }
                for (size_t k = 0; k < idle_fds.size(); ++k)
                    closeClient(idle_fds[k], connections, client_to_config, fds_to_remove, cgi_read_map, cgi_write_map);
            }

            int ret = poll(&poll_fds[0], poll_fds.size(), 1000);
            if (ret < 0) break;
            if (ret == 0) continue;

            for (size_t i = 0; i < poll_fds.size(); ++i)
            {
                int fd = poll_fds[i].fd;
                short revents = poll_fds[i].revents;
                if (revents == 0)
                    continue;
                if (isFdScheduledForRemoval(fds_to_remove, fd))
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
                            // FORCE NON-BLOCKING
                            int flags = fcntl(client_fd, F_GETFL, 0);
                            fcntl(client_fd, F_SETFL, flags | O_NONBLOCK);

                            // Create new connection object (Allocate on HEAP)
                            Connection* new_conn = new Connection(client_fd);
                            new_conn->touchActivity();
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
                        char buffer[32768];
                        ssize_t bytes = read(fd, buffer, sizeof(buffer));

                        if (bytes > 0)
                        {
                            conn->getCgiOutput().append(buffer, bytes);
                            conn->touchActivity();
                        }
                        else if (bytes == 0) // EOF
                        {
                            CgiHandler* cgi = conn->getCgiHandler();
                            if (cgi && cgi->getPid() > 0)
                            {
                                int status;
                                waitpid(cgi->getPid(), &status, 0);
                                cgi->clearPid();

                                // Determine HTTP status based on CGI exit status
                                if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
                                    // CGI exited with a non-zero status, indicating an error
                                    conn->out() = buildError(502, conn->request(), client_to_config[conn->fd()]);
                                } else if (WIFSIGNALED(status)) {
                                    // CGI was terminated by a signal (e.g., segfault, SIGKILL)
                                    conn->out() = buildError(502, conn->request(), client_to_config[conn->fd()]);
                                } else {
                                    // CGI exited successfully (status 0) or produced output
                                    std::string res = processCgiResponse(conn->getCgiOutput(), conn->request());
                                    if (res.empty())
                                        conn->out() = buildError(502, conn->request(), client_to_config[conn->fd()]);
                                    else
                                        conn->out() = res;
                                }
                            }
                            if (cgi)
                                closeCgiWritePipe(cgi, cgi_write_map, fds_to_remove);
                            conn->touchActivity(); // Give the server time to send the CGI result
                            if (cgi)
                            {
                                closeCgiReadPipe(cgi, cgi_read_map, fds_to_remove);
                                delete cgi;
                            }
                            else
                            {
                                ::close(fd);
                                cgi_read_map.erase(fd);
                                fds_to_remove.push_back(fd);
                            }
                            conn->setCgiHandler(NULL);
                            for (size_t k = 0; k < poll_fds.size(); ++k) {
                                if (poll_fds[k].fd == conn->fd()) {
                                    poll_fds[k].events = POLLIN | POLLOUT;
                                    break;
                                }
                            }
                        }
                    }
                    else if (revents & (POLLERR | POLLNVAL))
                    {
                        if (conn && conn->getCgiHandler())
                        {
                            closeCgiWritePipe(conn->getCgiHandler(), cgi_write_map, fds_to_remove);

                            conn->out() = buildError(502, conn->request(), client_to_config[conn->fd()]);

                            for (size_t k = 0; k < poll_fds.size(); ++k)
                            {
                                if (poll_fds[k].fd == conn->fd())
                                {
                                    poll_fds[k].events = POLLIN | POLLOUT;
                                    break;
                                }
                            }
                            closeCgiReadPipe(conn->getCgiHandler(), cgi_read_map, fds_to_remove);
                            delete conn->getCgiHandler();
                            conn->setCgiHandler(NULL);
                        }
                        else
                        {
                            ::close(fd);
                            cgi_read_map.erase(fd);
                            fds_to_remove.push_back(fd);
                        }
                    }
                }

                // ---------------------------------------------------------
                // CASE 3: CGI SCRIPT IS READY TO RECEIVE POST DATA
                // ---------------------------------------------------------
                else if (cgi_write_map.count(fd))
                {
                    Connection *conn = cgi_write_map[fd];
                    bool writeClosed = false;
                    if (revents & POLLOUT)
                    {
                        // 1. Get the data
                        const char* bodyPtr = conn->request().getBodyPtr();
                        size_t bytesToSend = conn->request().getBodyLen();
                        size_t bytesAlreadySent = conn->getCgiBytesWritten();

                        // 2. write whats left
                        size_t remaining = bytesAlreadySent < bytesToSend ? bytesToSend - bytesAlreadySent : 0;
                        if (remaining > 0)
                        {
                            size_t chunkSize = (remaining > 65536) ? 65536 : remaining;
                            ssize_t written = write(fd, bodyPtr + bytesAlreadySent, chunkSize);
                            if (written > 0) {
                                conn->addCgiBytesWritten(written);
                                conn->touchActivity();
                            }
                        }
                        
                        // 3. Close if done
                        if (!writeClosed && conn->getCgiBytesWritten() >= bytesToSend)
                        {
                            if (conn && conn->getCgiHandler())
                                conn->getCgiHandler()->closeWriteFd();
                            cgi_write_map.erase(fd);
                            fds_to_remove.push_back(fd);
                            writeClosed = true;
                        }
                    }
                    // handle Errors (POLLERR | POLLHUP) without POLLOUT
                    if (!writeClosed && (revents & (POLLERR | POLLHUP | POLLNVAL)))
                    {
                        if (conn && conn->getCgiHandler())
                        {
                            CgiHandler* cgi = conn->getCgiHandler();

                            closeCgiReadPipe(cgi, cgi_read_map, fds_to_remove);
                            closeCgiWritePipe(cgi, cgi_write_map, fds_to_remove);
                            conn->out() = buildError(502, conn->request(), client_to_config[conn->fd()]);
                            for (size_t k = 0; k < poll_fds.size(); ++k)
                            {
                                if (poll_fds[k].fd == conn->fd())
                                {
                                    poll_fds[k].events = POLLIN | POLLOUT;
                                    break;
                                }
                            }
                            delete cgi;
                            conn->setCgiHandler(NULL);
                        }
                        else
                        {
                            ::close(fd);
                            cgi_write_map.erase(fd);
                            fds_to_remove.push_back(fd);
                        }
                    }
                }
                else
                {
                    if (connections.find(fd) == connections.end())
                    {
                        closeClient(fd, connections, client_to_config, fds_to_remove, cgi_read_map, cgi_write_map);
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
                        closeClient(fd, connections, client_to_config, fds_to_remove, cgi_read_map, cgi_write_map);
                }
            }

            // Clean up closed FDs from poll_fds
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
        //return 1;
    }

    std::cout << "\nServer shutting down..." << std::endl;
    return 0;
}
