#pragma once

#include <string>
#include <unistd.h>
#include <ctime>
#include "Request.hpp"

#define MAX_HEADER_SIZE 32768 // 32KB

class CgiHandler;

class Connection
{
private:
    int         fd_;
    std::string inBuffer_;
    std::string outBuffer_;
    size_t      outBufferOffset_;
    Request     request_;
    bool        closed_;

    CgiHandler* cgiHandler_;     // Pointer to the active CGI handler (NULL if no CGI)
    std::string cgiOutput_;      // Buffer to hold data read from CGI stdout
    size_t cgiBytesWritten_;     // How much of the body we've written to CGI stdin
    time_t      cgiStartTime_;
    time_t      lastActivity_;

public:
    explicit Connection(int fd);
    ~Connection();
    void    clearIo();

    time_t       getCgiStartTime() const { return cgiStartTime_; }
    void         setCgiStartTime(time_t t) { cgiStartTime_ = t; }
    time_t       getLastActivity() const { return lastActivity_; }
    void         touchActivity() { lastActivity_ = time(NULL); }

    // Getters
    int  fd() const;
    std::string& in();
    std::string& out();
    Request&        request();
    const Request&  request() const;

    // Connection state
    bool isClosed() const;

    // Socket ops
    void closeNow();
    int  readFromSocket();
    int  writeToSocket();

    // CGI
    CgiHandler*     getCgiHandler() { return cgiHandler_; }
    void            setCgiHandler(CgiHandler* cgi) { cgiHandler_ = cgi; }
    std::string&    getCgiOutput() { return cgiOutput_; }
    size_t          getCgiBytesWritten() const { return cgiBytesWritten_; }
    void            addCgiBytesWritten(size_t bytes) { cgiBytesWritten_ += bytes; }

};
