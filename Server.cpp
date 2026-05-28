#include "Server.hpp"
#include <cerrno>
#include <cstring>
#include <sys/socket.h> 
#include <fcntl.h>
#include <arpa/inet.h>

Server::Server(const ServerConfig& config)
: config_(config), listen_fd_(-1)
{}

Server::~Server()
{
    if (listen_fd_ >= 0)
        ::close(listen_fd_);
}

/**
 *  Creates, configures, binds, and listens on a server socket.
 *
 * This function encapsulates the standard boilerplate for setting up a listening
 * socket for the server.
 * 1. Creates a TCP socket.
 * 2. Sets the `SO_REUSEADDR` option to allow the server to restart quickly.
 * 3. Makes the socket non-blocking (`O_NONBLOCK`) so that `accept()` doesn't freeze the server.
 * 4. Binds the socket to the host and port specified in the configuration.
 * 5. Puts the socket into listening mode to accept incoming connections.
 *
 * return `true` if all steps are successful, `false` otherwise.
 */
bool Server::bindAndListen()
{
    
    // 1. create socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0)
    {
        perror("socket");
        return false;
    }

    // 2. setting SO_REUSEADDR to restart server imediatly
    // without waiting for the port to timeout

    int one = 1;
    if (::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
    {
        perror("setsockopt");
        ::close(listen_fd_);
        return false;
    }

    // 3. Set Non-Blocking & Close-on-Exec
    // We combine F_SETFL (Status Flags like NonBlock) and F_SETFD (Descriptor Flags like CloExec)
    // O_NONBLOCK goes into SETFL. FD_CLOEXEC goes into SETFD.
    int flags = ::fcntl(listen_fd_, F_GETFL, 0);
    if (flags == -1 || ::fcntl(listen_fd_, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        perror("fcntl O_NONBLOCK");
        ::close(listen_fd_);
        return false;
    }

    int fd_flags = ::fcntl(listen_fd_, F_GETFD, 0);
    if (fd_flags == -1 || ::fcntl(listen_fd_, F_SETFD, fd_flags | FD_CLOEXEC) == -1)
    {
        perror("fcntl FD_CLOEXEC");
        ::close(listen_fd_);
        return false;
    }

    // 4. prepare address structure
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr(config_.host.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE)
    {
        fprintf(stderr, "Error: Invalid host IP %s\n", config_.host.c_str());
        ::close(listen_fd_);
        return false;
    }
    addr.sin_port = htons(config_.port);

    // 5. bind
    if (::bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        ::close(listen_fd_);
        return false;
    }

    if (::listen(listen_fd_, 128) < 0)
    {
        perror("listen");
        ::close(listen_fd_);
        return false;
    }
    return true;

}

int Server::acceptClient()
{
    struct sockaddr_in cli;
    socklen_t len = sizeof(cli);

    // Since listen_fd_ is now O_NONBLOCK, this will:
    // Return FD if client is waiting.
    // Return -1 (with errno EAGAIN) if no one is waiting.
    return ::accept(listen_fd_, (struct sockaddr*)&cli, &len);
}