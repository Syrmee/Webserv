#include "Request.hpp"

Request::Request() : isChunked_(false), chunkedCompleted_(false),
                    currentChunkSize_(0), chunkState_(0)
{
}
Request::~Request()
{}
Request::ParseError::ParseError(int status) : HttpError(status)
{}
Request::ParseError::~ParseError() throw()
{}

const std::string& Request::getPath(void) const
{
    return path_;
}
const std::string& Request::getQuery(void) const
{
    return query_;
}
const std::string& Request::getMethod(void) const
{
    return method_;
}
const std::string& Request::getVersion(void) const
{
    return version_;
}
const std::map<std::string, std::string>& Request::getHeaders() const
{
    return header_;
}

bool    Request::isChunked(void) const
{
    return isChunked_;
}
bool    Request::isChunkedCompleted() const
{
    return chunkedCompleted_;
}
std::string& Request::getUnchunkedBody()
{
    return unchunkedBody_; 
}
const std::string& Request::getUnchunkedBody() const
{
    return unchunkedBody_;
}
void Request::setUnchunkedBody(const std::string& b)
{
    unchunkedBody_ = b;
}

std::string Request::getValFromMap(std::string key) const
{
    key = toLower(key);
    std::map<std::string, std::string>::const_iterator it = header_.find(key);
    if (it != header_.end())
        return (it->second);
    return (std::string());
}
static bool isValidToken(const std::string& m)
{
    if (m.empty() || m.size() > 128) // m.size() > 128 is to limit big silly methods 
        return false;
    
    for (std::string::size_type i = 0; i < m.size(); ++i)
    {
        char c = m[i];
        if (c < 'A' || c > 'Z')
            return false;
    }
    return true;
}

/**
 *  Parses the HTTP method from the request line.
 *
 * Validates that the method is a valid token and checks if it is one of the
 * implemented methods (GET, POST, DELETE, HEAD). If the method is valid but not
 * implemented (e.g., PUT, TRACE), it returns a 501 Not Implemented error.
 */
int    Request::parseMethod()
{
    // std::cout << method_ << std::endl;
    if (!isValidToken(method_))
        return (400);

    std::string m[] = {"PUT", "CONNECT", "OPTIONS", "TRACE", "PATCH"};
    for(size_t i = 0; i < sizeof(m)/sizeof(m[0]); ++i)
    {
        if (method_ == m[i])
            return (501); // 501 not implimented
    }

    if (method_ == "GET" || method_ == "HEAD" || method_ == "POST" || method_ == "DELETE")
        return 0;

    return 501;
}


static std::string joinSegments(const std::vector<std::string>& segments)
{
    std::string r = "/";
    for (std::string::size_type i = 0; i < segments.size(); ++i)
    {
        r+= segments[i];
        if (i + 1 < segments.size())
            r += "/";
    }

    return (r);
}

static void splitOnSlash(const std::string& path, std::vector<std::string>& parts)
{
    std::string cur;

    for (std::string::size_type i = 0; i < path.size(); ++i)
    {
        if (path[i] == '/')
        {
            if (!cur.empty())
                { parts.push_back(cur); cur.clear(); }
        }
        else
            cur += path[i];
    }

    if (!cur.empty()) parts.push_back(cur);
}

static int normalizePath(std::string& path)
{
    std::vector<std::string> parts, stack;
    bool    hasSlash = (path.size() > 1 && path[path.size() - 1] == '/');
    splitOnSlash(path, parts);

    for (std::string::size_type i = 0; i < parts.size(); ++i)
    {
        const std::string& s = parts[i];
        if (s == "." || s.empty())
            continue;
        if (s == "..")
        {
            if (stack.empty())
                return 403; // it is forbiden to travers back with ".." beyond rootfolder.
            stack.pop_back();
        }
        else
            stack.push_back(s);
    }

    
    std::string norm = joinSegments(stack);
    if (hasSlash && norm != "/")
        norm += '/';
    
    path = norm;
    return (0);
}


/**
 *  Parses and normalizes the request URI.
 *
 * This function performs several critical tasks:
 * 1. Separates the path from the query string (e.g., "/path?query=1").
 * 2. Rejects requests containing forbidden characters or fragments (#).
 * 3. Validates that the path starts with a '/'.
 * 4. Calls `normalizePath` to resolve `.` and `..` segments, preventing
 *    path traversal attacks.
 * return 0 on success, or an HTTP error code on failure.
 */
int  Request::parsePath()
{
    if (path_.empty())
    {
        // std::cout << "path empty (before parse)" << std::endl;
        return (400);
    }

    std::string raw = path_;
    
    size_t pos = raw.find('#');
    if (pos != std::string::npos)
        return (400);  // fragments should not be sent to server. 

    pos = raw.find('?');
    if (pos != std::string::npos)
    {
        query_ = raw.substr(pos + 1);
        raw.erase(pos);
    }
    else
        query_.clear();
    
    path_ = raw;

    for (std::string::size_type i = 0; i < path_.size(); ++i)
    {
        if (static_cast<unsigned char>(path_[i]) < 0x20 || static_cast<unsigned char>(path_[i]) == 0x7f)
        {
            // std::cout << "path rejected for non-printable chars" << std::endl;
            return (400); // non printable chars are rejected for high security :) 0x20 = 32 = SPACE; 0x7f = DEL
        }
        if (static_cast<unsigned char>(path_[i]) >= 0x80) // rejection non-ASCII bytes 
            return (400);

    }
    
    if (path_.empty() || path_[0] != '/'){
        // std::cout << "path empty (after parse) or isn't starting with '/' " << std::endl;
        return 400;
    }

    int code = normalizePath(path_);
    if (code != 0)
        return (code);

    return (0);

}

/**
 *  Parses the HTTP protocol version from the request line.
 *
 * Ensures the version is one of the supported protocols (HTTP/1.0 or HTTP/1.1).
 * It also enforces the HTTP/1.1 rule that a "Host" header must be present.
 */
int    Request::parseVersion()
{
    if (version_ == "HTTP/1.1" && header_.find("host") == header_.end())
        return 400;
    if (version_ != "HTTP/1.0" && version_ != "HTTP/1.1")
        return  505;
    return 0;
}

/**
 *  Parses the entire HTTP request header.
 *
 * This is the main entry point for parsing an incoming request. It reads the
 * request from the provided buffer line by line.
 * 1. Parses the request line (Method, URI, Version).
 * 2. Calls the specific parsers for method, path, and version.
 * 3. Reads all subsequent header fields (e.g., "Host: example.com") into a map.
 * 4. Handles header value merging for fields that allow it (e.g., Cookie).
 * 5. Calls `validateHeaders` to perform security and consistency checks.
 * 6. Determines if the request has a chunked transfer encoding.
 *
 *  buffer A C-string containing the raw HTTP request headers.
 */
void    Request::parseHeader(const char *buffer)
{
    std::istringstream req(buffer);
    std::string line;
    HeaderLimits limits;
    int code = 0;
    
    // load 3 main header atributes : METHOD  PATH  PROTOCOL*(HTTP/1.1)(HTTP/1.0)
    if (std::getline(req, line))
    {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        
        if (line.size() > limits.max_line)
            throw ParseError(400); // whole request line is too long -> 400 / could be 414 but its for URI to be exact.

        std::istringstream iss(line);
        
        // Extract method (if missing → 400)
        if (!(iss >> method_))
            throw ParseError(400);

        // Extract path (request-target). If missing → 400.
        if (!(iss >> path_))
            throw ParseError(400);

        // If the request-target is too long → 414 (URI)
        if (path_.size() > limits.max_uri)
            throw ParseError(414);

        // Extract HTTP version (if missing → 400)
        if (!(iss >> version_))
            throw ParseError(400);
        
        // Extra garbage. (400 Bad request)
        std::string extra;
        if (iss >> extra)
            throw ParseError(400);
    }
    else
    {
        throw ParseError(400);
    }

    code = parseMethod();
    // std::cout << "::::" << code << "::::" << std::endl;
    if (code != 0)
        throw ParseError(code);

    code = parsePath();
    if (code != 0)
        throw ParseError(code);
    
    // std::cout << path_ << " : " << query_ << std::endl; // for testing !

    // load map with "key" : "value" ; example "host" : "local" , "connection" : "close";
    while (std::getline(req, line))
    {
        if (!line.empty() && line[line.size() - 1] == '\r')
            line.erase(line.size() - 1);
        
        if (line == "\r" || line.empty()) //end of header 
            break;
        
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
            throw ParseError(400);
        
        if (line.size() > limits.max_line)
            throw ParseError(431);

        size_t pos = line.find(':');
        if (pos == std::string::npos)
            throw ParseError(400);

        // reject whitespace before colon (invalid per RFC)
        if (pos > 0 && (line[pos - 1] == ' ' || line[pos - 1] == '\t'))
            throw ParseError(400);

        std::string key = trim(line.substr(0, pos));
        std::string value = trim(line.substr(pos + 1));

        // reject empty header names
        if (key.empty())
            throw ParseError(400);

        key = toLower(key);
        //value = toLower(value);

        // reject CR/LF smuggling inside value
        if (value.find('\r') != std::string::npos || value.find('\n') != std::string::npos)
            throw ParseError(400);
        
        // reject duplicates 
        HeaderDuplicateCheckAndAppend(key, value);
    }

    code = validateHeaders(limits);
    if (code != 0)
        throw ParseError(code);

    code = parseVersion();
    if (code != 0)
        throw ParseError(code);
    
    std::string te = getValFromMap("transfer-encoding");
    te = toLower(te);
    if (te.find("chunked") != std::string::npos)
    {
        isChunked_ = true;
        chunkedCompleted_ = false;
        chunkState_ = 0;
        currentChunkSize_ = 0;
    }
    
    //printMap(); // for testing !
}

/**
 *  Parses a portion of a chunked request body.
 *
 * This function implements a state machine to decode a chunked transfer stream.
 * It can be called multiple times as new data arrives on the socket.
 * - State 0: Expects a line with the hexadecimal size of the next chunk.
 * - State 1: Expects to read the raw data of the chunk itself.
 * - State 2: Expects the final empty chunk and trailing CRLF.
 *
 * rawData The raw data buffer from the socket, which is consumed as it's parsed.
 *  decodedBody The output string where the clean, decoded body is appended.
 * return `true` if the entire chunked body has been successfully parsed, `false` otherwise.
 */
bool Request::parseChunkedBody(std::string& rawData, std::string& decodedBody)
{
    while (!rawData.empty() && !chunkedCompleted_)
    {
        // ---------------------------------------------------------
        // STATE 0: Read the Hexadecimal Size
        // ---------------------------------------------------------
        if (chunkState_ == 0)
        {
            size_t pos = rawData.find("\r\n");
            if (pos == std::string::npos)
                return false; // Not enough data yet, wait for the next poll()

            // Extract the line containing the hex size
            std::string hexLine = rawData.substr(0, pos);
            
            // HTTP/1.1 allows "chunk extensions" (e.g., "1A;name=value"). 
            // We must ignore everything after the semicolon.
            size_t semiPos = hexLine.find(';');
            if (semiPos != std::string::npos)
                hexLine = hexLine.substr(0, semiPos);

            // Convert hex string to integer size
            std::stringstream ss;
            ss << std::hex << trim(hexLine);
            if (!(ss >> currentChunkSize_))
                throw ParseError(400); // Invalid chunk size format

            // Remove the size line and the "\r\n" from the raw buffer
            rawData.erase(0, pos + 2);

            if (currentChunkSize_ == 0)
            {
                // A size of 0 marks the end of the chunks
                chunkState_ = 2; 
            }
            else
            {
                // We have a valid size, move to reading data
                chunkState_ = 1;
            }
        }
        // ---------------------------------------------------------
        // STATE 1: Read the Chunk Data
        // ---------------------------------------------------------
        else if (chunkState_ == 1)
        {
            // We wait until we have the full chunk data PLUS the trailing "\r\n"
            if (rawData.size() < currentChunkSize_ + 2)
                return false; 

            // Extract exactly 'currentChunkSize_' bytes and append to our clean output
            decodedBody += rawData.substr(0, currentChunkSize_);

            // Erase the data AND the trailing "\r\n" from the raw socket buffer
            rawData.erase(0, currentChunkSize_ + 2);

            // Go back to expecting a new size
            chunkState_ = 0;
        }
        // ---------------------------------------------------------
        // STATE 2: Read Final CRLF
        // ---------------------------------------------------------
        else if (chunkState_ == 2)
        {
            // The final "0\r\n" is followed by an empty line "\r\n" (or trailing headers)
            size_t pos = rawData.find("\r\n");
            if (pos == std::string::npos)
                return false;

            // Remove the final "\r\n"
            rawData.erase(0, pos + 2);
            
            chunkedCompleted_ = true;
            return true; // We are completely done parsing the chunked body!
        }
    }

    return chunkedCompleted_;
}

void    Request::reset()
{
    method_.clear();
    path_.clear();
    query_.clear();
    version_.clear();
    header_.clear();
    unchunkedBody_.clear();

    isChunked_ = false;
    chunkedCompleted_ = false;
    currentChunkSize_ = 0;
    chunkState_ = 0;
}
std::string Request::trim(std::string str)
{
    if (!str.empty() && str.find_first_not_of(" \t") != std::string::npos)
    {
        str.erase(str.find_last_not_of(" \t") + 1);
        str.erase(0, str.find_first_not_of(" \t"));
    }
    return str;
}
