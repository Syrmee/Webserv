#pragma once

#include "Request.hpp"
#include "Config.hpp"
#include "HttpError.hpp"
#include <string>
#include <map>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>

class CgiHandler {
private:
    std::map<std::string, std::string> envMap_;
    char** envp_;
    
    int pipeIn_[2];  // Server -> CGI (stdin)
    int pipeOut_[2]; // CGI -> Server (stdout)
    pid_t cgiPid_;

    std::string scriptPath_;
    std::string workingDir_;
    std::string interpreterPath_;
    const Location& loc_;
    std::string scriptName_;
    std::string pathInfo_;

    // Private helpers
    void initEnv(const Request& req);
    char** mapToEnvp();
    void freeEnvp();

public:
    CgiHandler(const Request& req, const Location& loc, const std::string& scriptPath, const std::string& scriptName, const std::string& pathInfo, std::string interpreter);
    ~CgiHandler();

    // The function that actually calls fork() and execve()
    void executeCgi(); 
    
    // Getters so your webserv.cpp can monitor these pipes with poll()
    int getReadFd() const { return pipeOut_[0]; }
    int getWriteFd() const { return pipeIn_[1]; }
    pid_t getPid() const { return cgiPid_; }
    void    clearPid() { cgiPid_ = -1; }
};