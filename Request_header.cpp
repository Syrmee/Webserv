#include "Request.hpp"


static bool isAscii(char c)
{
    return (unsigned char)c <= 0x7F;
}

static bool isValidHeaderName(const std::string& s)
{
    if (s.empty())
        return false;
    for (std::string::size_type i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (!isAscii(c))
            return false;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

// OWS = SP / HTAB
static std::string trimOWS(const std::string& s)
{
    std::string::size_type b = 0, e = s.size();

    while (b < e && (s[b] == ' ' || s[b] == '\t'))
        ++b;
    while (e > b && (s[e-1] == ' ' || s[e-1] == '\t'))
        --e;
    
    return s.substr(b, e - b);
}

static bool hasCtlExceptHTab(const std::string& s)
{
    for (std::string::size_type i = 0; i < s.size(); ++i)
    {
        unsigned char uc = (unsigned char)s[i];
        if (uc == 0)
            return true;                 // NUL
        if (uc < 0x20 && uc != 0x09)
            return true; // control except HTAB
        if (uc == 0x7F)
            return true;              // DEL
    }
    return false;
}

static bool parseDecimalSizeT(const std::string& s, size_t& out)
{
    if (s.empty())
        return false;
    size_t v = 0;
    for (std::string::size_type i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (c < '0' || c > '9')
            return false;
        size_t d = (size_t)(c - '0');
        // crude overflow guard (portable for size_t)
        if (v > (size_t(-1) / 10))
            return false;
        v = v * 10 + d;
        if (v < d)
            return false; // overflow paranoia
    }
    out = v;
    return true;
}

size_t  Request::getContentLength() const
{
    const std::string cl = getValFromMap("content-length");
    if (cl.empty())
        return (0);
    
    size_t len = 0;
    if (!parseDecimalSizeT(cl, len))
        return (0);
    
    return (len);
}

/**
 *  Appends a header to the internal map, handling duplicates correctly.
 *
 * According to RFC 7230, some headers like `Host` or `Content-Length` MUST NOT
 * appear more than once. This function enforces this by throwing a 400 error
 * if a duplicate is found.
 *
 * For other headers, it correctly merges values. It uses a comma-separated
 * list for general headers and a semicolon-separated list specifically for
 * the `Cookie` header, as required by their respective RFCs.
 *
 *  key The lowercase header field name.
 *  value The header field value.
 */

void    Request::HeaderDuplicateCheckAndAppend(const std::string &key, const std::string &value)
{
    if (header_.find(key) == header_.end())
    {
        header_[key] = value;
        return ;
    }

    if ( key == "host" || key == "content-length" || key == "content-type" )
        throw ParseError(400);

    if ( key == "cookie" )
        header_[key] += "; " + value;
    else
        header_[key] += ", " + value;
}

/**
 *  Validates all parsed headers against configured limits and RFC rules.
 *
 * This critical function performs several checks after all headers are parsed:
 * 1. Enforces limits on the total number of headers and their approximate total size.
 * 2. Ensures the `Host` header is present for HTTP/1.1 requests and is not duplicated.
 * 3. Prevents HTTP Request Smuggling by rejecting control characters in header values.
 * 4. Enforces consistency between `Transfer-Encoding` and `Content-Length`. A request
 *    cannot have both `Transfer-Encoding: chunked` and a `Content-Length`.
 * 5. Validates that the `Content-Length` value, if present, is a valid non-negative
 *    number and does not exceed the configured maximum body size.
 *
 *  lim A struct containing the configured header limits.
 * return 0 on success, or an appropriate HTTP error code on failure.
 */


int Request::validateHeaders(const HeaderLimits& lim)
{
    // 1) Count/size limits
    if (header_.size() > lim.max_fields)
        return 431;

    size_t approx_total = 0;
    bool sawHost = false;

    for (std::map<std::string, std::string>::const_iterator it = header_.begin();
         it != header_.end(); ++it)
    {
        const std::string& name  = it->first;   // already stored as lowercase
        const std::string& value = it->second;

        approx_total += name.size() + value.size() + 2; // rough
        if (approx_total > lim.max_total)
            return 431;

        // Field-name syntax
        if (!isValidHeaderName(name))
        {
            std::cout << "non valid header";
            return 400;
        }
        
        if (hasCtlExceptHTab(name) || hasCtlExceptHTab(value))
            throw ParseError(400);

        // Track Host presence
        if (name == "host")
        {
            if (sawHost)
                return 400;  // duplicate Host
            if (trimOWS(value).empty())
                return 400;  // empty Host
            sawHost = true;
        }
    }

    // HTTP/1.1 requires Host
    if (version_ == "HTTP/1.1" && !sawHost)
        return 400;

    // 2) TE vs CL consistency
    const std::string te = toLower(getValFromMap("transfer-encoding"));
    const std::string cl = toLower(getValFromMap("content-length"));

    if (!te.empty() && te != "identity")
    {
        if (te == "chunked")
        {
            if (!cl.empty())
                return 400;
        }
        else
            return 501;

    }
    if (!cl.empty())
    {
        // Content-Length must be a single valid non-negative decimal
        size_t clen = 0;
        if (!parseDecimalSizeT(cl, clen))
            return 400;
        if (clen > lim.max_body)
            return 413;
    }

    return 0;
}

