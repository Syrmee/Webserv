#include "Connection.hpp"
#include "CgiHandler.hpp"
#include <sys/socket.h>  // for recv(), send()
#include <iostream>
#include <cerrno>

Connection::Connection(int fd)
    : fd_(fd), outBufferOffset_(0), closed_(false),
    cgiHandler_(NULL), cgiBytesWritten_(0)
{}

Connection::~Connection()
{
    if (cgiHandler_)
        delete cgiHandler_;

    if (!closed_ && fd_ >= 0)
    {
        ::close(fd_);
        closed_ = true;
    }
}

int Connection::fd() const
{
    return fd_;
}

std::string& Connection::in()
{
    return inBuffer_;
}

std::string& Connection::out()
{
    return outBuffer_;
}

Request& Connection::request()
{
    return request_;
}

const Request& Connection::request() const
{
    return request_;
}

bool Connection::isClosed() const
{
    return closed_;
}

void Connection::closeNow()
{
    if (!closed_ && fd_ >= 0)
    {
        ::close(fd_);
        closed_ = true;
    }
}

void Connection::clearIo()
{
    inBuffer_.clear();
    outBuffer_.clear();
    outBufferOffset_ = 0;
}


/**
 *  Reads data from the socket into the connection's input buffer.
 *
 * Performs a non-blocking read from the connection's file descriptor.
 * return The number of bytes read, 0 if the client disconnected gracefully,
 * or -1 on a socket error (which closes the connection).
 */
int Connection::readFromSocket()
{
    if (inBuffer_.size() > MAX_HEADER_SIZE && inBuffer_.find("\r\n\r\n") == std::string::npos) // Fix potential issue that could break legitimate upload.
        throw Request::ParseError(431);

    char buf[8192];
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);

    if (n > 0)
    {
        inBuffer_.append(buf, n);
        return static_cast<int>(n);
    }
    else if (n == 0)
        return 0;

    perror("recv()");
    closeNow();
    return -1;
}


/**
 *  Writes data from the connection's output buffer to the socket.
 *
 * Performs a non-blocking write to the connection's file descriptor. It keeps
 * track of how much data has been sent and resumes writing from where it left
 * off on subsequent calls.
 * return The number of bytes written, or -1 on a socket error.
 */
int Connection::writeToSocket()
{
    if (outBuffer_.empty() || outBufferOffset_ >= outBuffer_.size())
    {
        outBuffer_.clear();
        outBufferOffset_ = 0;
        cgiOutput_.clear();
        return 0;
    }
    
    size_t totalSize = outBuffer_.size();
    size_t remaining = totalSize - outBufferOffset_;

    // Pointer arithmetic: Start sending from where we left off
    const char* dataPtr = outBuffer_.c_str() + outBufferOffset_;

    ssize_t n = ::send(fd_, dataPtr, remaining, 0);

    if (n >= 0)
    {
        outBufferOffset_ += n;
        if (outBufferOffset_ >= totalSize)
        {
            outBuffer_.clear();
            outBufferOffset_ = 0;
            cgiOutput_.clear();
        }
        return static_cast<int>(n);
    }

    perror("send()");
    closeNow();
    return -1;
}
