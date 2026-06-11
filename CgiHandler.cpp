#include "CgiHandler.hpp"
#include <sstream>
#include <cctype>
#include <iostream>
 
CgiHandler::CgiHandler(const Request& req, const Location& loc, const std::string& scriptPath, const std::string& scriptName, const std::string& pathInfo, std::string interpreter) 
    : envp_(NULL), cgiPid_(-1), scriptPath_(scriptPath), interpreterPath_(interpreter), loc_(loc), scriptName_(scriptName), pathInfo_(pathInfo)
{
    pipeIn_[0] = -1; pipeIn_[1] = -1; // Server -> CGI
    pipeOut_[0] = -1; pipeOut_[1] = -1; // CGI -> Server

    // Per the subject, CGI scripts must be executed in the directory where they
    // are located. This allows them to use relative paths to access other files.
    // So we extract the directory path from the script path.
    size_t lastSlash = scriptPath_.find_last_of('/');
    if (lastSlash != std::string::npos)
        workingDir_ = scriptPath_.substr(0, lastSlash);
    else
        workingDir_ = ".";

    initEnv(req);
    envp_ = mapToEnvp();
}

CgiHandler::~CgiHandler()
{
    freeEnvp();

    if (cgiPid_ > 0)
    {
        kill(cgiPid_, SIGKILL);
        waitpid(cgiPid_, NULL, 0);
    }

    closeFd(pipeIn_[0]);
    closeFd(pipeIn_[1]);
    closeFd(pipeOut_[0]);
    closeFd(pipeOut_[1]);
}

void CgiHandler::closeFd(int& fd)
{
    if (fd != -1)
    {
        close(fd);
        fd = -1;
    }
}

void CgiHandler::closeReadFd()
{
    closeFd(pipeOut_[0]);
}

void CgiHandler::closeWriteFd()
{
    closeFd(pipeIn_[1]);
}

/**
 *  Initializes the environment variables for the CGI script.
 *
 * This function populates a map with all necessary CGI/1.1 (RFC 3875) variables,
 * including server information, request details, and all client-provided HTTP
 * headers prefixed with "HTTP_". This environment is crucial for the script to
 * understand the context of the request.
 */
void CgiHandler::initEnv(const Request& req)
{
    // 1. Mandatory CGI 1.1 Variables
    envMap_["GATEWAY_INTERFACE"] = "CGI/1.1";
    envMap_["SERVER_PROTOCOL"]   = req.getVersion();
    envMap_["SERVER_SOFTWARE"]   = "webserv/0.1";
    envMap_["QUERY_STRING"]      = req.getQuery();
    envMap_["REQUEST_URI"]       = req.getPath() + (req.getQuery().empty() ? "" : "?" + req.getQuery());
    
    envMap_["REQUEST_METHOD"]    = req.getMethod();
    envMap_["SERVER_NAME"]       = "127.0.0.1";
    envMap_["SERVER_PORT"]       = "8080";
    envMap_["REMOTE_ADDR"]       = "127.0.0.1";
    
    // Required by PHP-CGI
    envMap_["SCRIPT_FILENAME"]   = scriptPath_; 
    envMap_["REDIRECT_STATUS"]   = "200";

    // Set SCRIPT_NAME and PATH_INFO based on the resolved path from webserv.cpp
    envMap_["SCRIPT_NAME"]       = scriptName_;
    envMap_["PATH_INFO"]         = pathInfo_.empty() ? req.getPath() : pathInfo_;

    // PATH_TRANSLATED is the filesystem path for the PATH_INFO.
    if (!pathInfo_.empty()) {
        std::string pathTranslated = loc_.root;
        if (!pathTranslated.empty() && pathTranslated[pathTranslated.length() - 1] == '/')
            pathTranslated.erase(pathTranslated.length() - 1);
        envMap_["PATH_TRANSLATED"] = pathTranslated + pathInfo_;
    }

    // 2. Handle the Body Size
    if (req.getMethod() == "POST" || req.getMethod() == "PUT")
    {
        std::stringstream ss;
        ss << req.getBodyLen(); 
        envMap_["CONTENT_LENGTH"] = ss.str();
        
        std::string contentType = req.getValFromMap("content-type");
        if (!contentType.empty())
            envMap_["CONTENT_TYPE"] = contentType;
    }

    // 3. Dynamically map ALL client headers to HTTP_ variables
    const std::map<std::string, std::string>& headers = req.getHeaders();
    for (std::map<std::string, std::string>::const_iterator it = headers.begin(); it != headers.end(); ++it)
    {
        std::string key = it->first; // e.g., "user-agent"
        
        // Convert to UPPERCASE and replace '-' with '_'
        for (size_t i = 0; i < key.length(); ++i)
        {
            if (key[i] == '-') 
                key[i] = '_';
            else 
                key[i] = std::toupper(key[i]);
        }
        
        // Exclude specific headers
        if (key != "CONTENT_TYPE" && key != "CONTENT_LENGTH" && key != "TRANSFER_ENCODING")
            envMap_["HTTP_" + key] = it->second;
    }
}

char** CgiHandler::mapToEnvp()
{
    char** env = new char*[envMap_.size() + 1];
    int i = 0;
    for (std::map<std::string, std::string>::iterator it = envMap_.begin(); it != envMap_.end(); ++it)
    {
        std::string envStr = it->first + "=" + it->second;
        env[i] = new char[envStr.size() + 1];
        std::strcpy(env[i], envStr.c_str());
        i++;
    }
    env[i] = NULL;
    return env;
}

void CgiHandler::freeEnvp()
{
    if (envp_)
    {
        for (int i = 0; envp_[i] != NULL; ++i)
            delete[] envp_[i];
        delete[] envp_;
        envp_ = NULL;
    }
}

/**
 *  Executes the CGI script in a separate child process.
 *
 * This function is the core of the CGI handling. It performs the following steps:
 * 1. Creates two pipes for bidirectional communication: one for the server to send
 *    the request body (stdin) and one for the server to receive the script's
 *    output (stdout).
 * 2. Forks the server process.
 * 3. In the child process, it redirects stdin and stdout to the pipes, changes
 *    the working directory, and then uses `execve` to replace the process image
 *    with the CGI script interpreter.
 * 4. In the parent process, it closes the unused ends of the pipes and sets its
 *    ends to non-blocking mode to integrate with the main `poll()` loop.
 */
void CgiHandler::executeCgi()
{
    // 1. Create the two pipes
    // pipeIn: Server writes to [1], CGI reads from [0]
    if (pipe(pipeIn_) < 0)
        throw HttpError(500); // Internal Server Error
    
    // pipeOut: CGI writes to [1], Server reads from [0]
    if (pipe(pipeOut_) < 0)
    {
        closeFd(pipeIn_[0]);
        closeFd(pipeIn_[1]);
        throw HttpError(500);
    }

    // 2. Fork the process
    cgiPid_ = fork();
    if (cgiPid_ < 0)
    {
        closeFd(pipeIn_[0]);
        closeFd(pipeIn_[1]);
        closeFd(pipeOut_[0]);
        closeFd(pipeOut_[1]);
        throw HttpError(500);
    }

    // ==========================================================
    // 3. CHILD PROCESS (The CGI Script)
    // ==========================================================
    if (cgiPid_ == 0)
    {
        // A. Close the pipe ends the child doesn't need
        close(pipeIn_[1]);  // Child doesn't write to its own stdin
        close(pipeOut_[0]); // Child doesn't read its own stdout

        // B. Redirect Standard Input & Output using dup2
        dup2(pipeIn_[0], STDIN_FILENO);
        dup2(pipeOut_[1], STDOUT_FILENO);

        // C. Close the original file descriptors (they are duplicated now)
        close(pipeIn_[0]);
        close(pipeOut_[1]);

        // D. Change working directory (Subject requirement for relative paths)
        if (chdir(workingDir_.c_str()) != 0)
        {
            // If chdir fails, we must exit the child process immediately.
            _exit(1); 
        }

        // E. Setup argv for execve
        // Conventionally, argv[0] is the path to the program itself.
        char* argv[3];
        argv[0] = const_cast<char*>(interpreterPath_.c_str());
        argv[1] = const_cast<char*>(scriptPath_.c_str());
        argv[2] = NULL;

        // F. Execute the script!
        // If this succeeds, the child process is replaced by the script.
        execve(interpreterPath_.c_str(), argv, envp_);

        // G. If execve returns, it means it FAILED (e.g., file not found/permissions)
        // The child process MUST terminate to prevent a fork bomb.
        _exit(1); 
    }
    
    // ==========================================================
    // 4. PARENT PROCESS (The Web Server)
    // ==========================================================
    else
    {
        // A. Close the pipe ends the server doesn't need
        closeFd(pipeIn_[0]);  // Server doesn't read from CGI's stdin
        closeFd(pipeOut_[1]); // Server doesn't write to CGI's stdout

        // B. Make the Server's ends NON-BLOCKING
        fcntl(pipeIn_[1], F_SETFL, O_NONBLOCK);
        fcntl(pipeOut_[0], F_SETFL, O_NONBLOCK);
    }
}
