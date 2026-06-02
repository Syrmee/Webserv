#pragma once

#include <string>
#include <unistd.h>
#include "Request.hpp"

#define MAX_HEADER_SIZE 32768 // 32KB

#define TIMEOUT_SECONDS 10

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

    time_t establishementTimestamp_; // When the connection was established
    bool timedout_;

public:
    explicit Connection(int fd);
    ~Connection();
    void    clearIo();

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

    long getEstablishementTimestamp() {return establishementTimestamp_;};

    bool isTimedout() {return timedout_;};
    void timeout() {timedout_ = true;};
};
