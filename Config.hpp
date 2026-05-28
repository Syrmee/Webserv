#pragma once

#include <string>
#include <vector>
#include <map>
#include <iostream>

/**
 *  Represents a `location` block within the configuration.
 * Each location defines a set of rules for a specific URI prefix.
 */
struct Location {
    // The URI prefix that this location block will match, e.g., `/` or `/images`.
    std::string                 uri;

    // The root directory on the filesystem to search for requested files.
    std::string                 root;

    // The default file to serve if the request URI is a directory, e.g., `index.html`.
    std::string                 index;

    // If true, generates a directory listing page when a directory is requested
    // and no index file is found.
    bool                        autoindex;

    // A list of HTTP methods allowed for this location (e.g., GET, POST).
    std::vector<std::string>    methods;

    // If set, all requests to this location will be redirected to `return_url`
    // with the specified `return_code`.
    std::string                 return_url;
    int                         return_code;

    // The directory on the filesystem where uploaded files (via POST) should be stored.
    std::string                 upload_store;

    // Configures CGI handling. `cgi_extension` is the file extension that triggers
    // CGI (e.g., `.py`), and `cgi_path` is the path to the CGI interpreter.
    std::string                 cgi_extension;
    std::string                 cgi_path;

    // Overrides the server-level `client_max_body_size` for this specific location.
    size_t                      client_max_body_size;

    // Constructor to set safe defaults
    Location() : autoindex(false), return_code(0), client_max_body_size(0) {}
};

/**
 *  Represents a `server` block from the configuration file.
 * This struct holds all configuration for a single virtual server, which listens
 * on a unique host and port combination.
 */
struct ServerConfig {
    // The host IP and port that this server block will listen on.
    int                         port;
    std::string                 host;

    // The server name, used by clients to identify the server (e.g., `example.com`).
    std::string                 server_name;

    // A map to specify custom error pages for specific HTTP status codes.
    std::map<int, std::string>  error_pages;

    // The default maximum size for a client request body in bytes.
    // Can be overridden by individual locations.
    size_t                      client_max_body_size;

    // A list of all location blocks defined within this server.
    std::vector<Location>       locations;

    // Constructor with subject-compliant defaults
    ServerConfig() : port(0), host("127.0.0.1"), client_max_body_size(1000000) {}
};

/**
 *  The main configuration class that orchestrates parsing.
 * This class acts as the top-level container for the entire server configuration.
 * Its primary role is to parse the config file and store the resulting server blocks.
 */
class Config {
public:
    // A vector containing all the server configurations parsed from the file.
    std::vector<ServerConfig> servers;

    Config();
    ~Config();

    // Parses the specified configuration file and populates the `servers` vector.
    void    parse(const std::string& filename);
};
