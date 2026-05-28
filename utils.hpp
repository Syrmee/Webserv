#pragma once

#include <string>
#include "HttpError.hpp"
#include "Config.hpp"

class Request;

// Filesystem helpers
std::string resolveFullPath(const Location& loc, const std::string& path);
std::string buildDirectoryListing(const Request& req, const std::string& fsPath);

// Date helper
std::string httpDateNow();

// Generic utility
std::string toLower(const std::string& key);
bool isDir(const std::string& path);
bool isFile(const std::string& path);
bool isReadable(const std::string& path);

// More 
bool        shouldKeepAlive(const Request& request, int status);
std::string buildError(int code, const Request& request, const ServerConfig* cfg, const std::string& allowedMethods = "");
std::string buildRedirect(const Request& request, int status, const std::string& location);
std::string buildSuccess(const Request& req,const std::string& fsPath, const ServerConfig* cfg);
std::string trim(const std::string& str);