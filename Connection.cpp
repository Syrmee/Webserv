#include "Connection.hpp"
#include "CgiHandler.hpp"
#include <sys/socket.h>  // for recv(), send()
#include <iostream>

Connection::Connection(int fd)
    : fd_(fd), outBufferOffset_(0), closed_(false),
    cgiHandler_(NULL), cgiBytesWritten_(0), cgiStartTime_(0), lastActivity_(time(NULL))
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
    request_.reset();
}

int Connection::readFromSocket()
{
    if (inBuffer_.size() > MAX_HEADER_SIZE && inBuffer_.find("\r\n\r\n") == std::string::npos)
        throw Request::ParseError(431);

    char buf[65536]; // Increased buffer size to 64KB for high-throughput uploads
    ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);

    if (n > 0)
    {
        inBuffer_.append(buf, n);
        return static_cast<int>(n);
    }
    else if (n == 0)
        return 0;

    // Return -2 to indicate EAGAIN/EWOULDBLOCK
    return -2;
}

int Connection::writeToSocket()
{
    if (outBuffer_.empty() || outBufferOffset_ >= outBuffer_.size())
    {
        std::string().swap(outBuffer_);
        outBufferOffset_ = 0;
        std::string().swap(cgiOutput_);
        request_.reset();
        return 0;
    }
    
    size_t totalSize = outBuffer_.size();
    size_t remaining = totalSize - outBufferOffset_;

    const char* dataPtr = outBuffer_.c_str() + outBufferOffset_;
    ssize_t n = ::send(fd_, dataPtr, remaining, 0);

    if (n >= 0)
    {
        outBufferOffset_ += n;
        if (outBufferOffset_ >= totalSize)
        {
            std::string().swap(outBuffer_);
            outBufferOffset_ = 0;
            std::string().swap(cgiOutput_);
            request_.reset();
        }
        return static_cast<int>(n);
    }

    // Return -2 on EAGAIN/EWOULDBLOCK
    return -2;
}
