﻿#pragma once
#include <iostream>
#include <sstream>
#include <string>
#include <map>
#include <boost/asio.hpp>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/asio/spawn.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>




//log for debug
//if need, define macro HAS_HTTP_CLIENT_LOG

#define HTTP_CLIENT_INFO     HTTP_CLIENT_LOG(1)
#define HTTP_CLIENT_WARN     HTTP_CLIENT_LOG(2)
#define HTTP_CLIENT_ERROR    HTTP_CLIENT_LOG(3)

#define HTTP_CLIENT_LOG(_level) CAsyncHttpClientLog<_level>(__FILE__, __LINE__).stream()

//for simple log, I use int directly other than enum
template<int _level>
class CAsyncHttpClientLog
{
public:
    CAsyncHttpClientLog(const char* file, const int line)
        : m_file_name(file)
        , m_line(line)
    {
        size_t pos = m_file_name.find_last_of("/\\");
        if (std::string::npos != pos)
        {
            m_file_name = m_file_name.substr(pos);
            boost::algorithm::trim_if(m_file_name, boost::algorithm::is_any_of("/\\"));
        }
    }

    ~CAsyncHttpClientLog()
    {
        std::string level;
        switch (_level)
        {
        case 1:
            level = "INFO  ";
            break;

        case 2:
            level = "WARN  ";
            break;

        case 3:
            level = "ERROR ";
            break;

        default:
            level = "ERROR ";
            break;
        }

#if defined(HAS_HTTP_CLIENT_LOG)
        std::cout << level.c_str() << "[" << m_file_name.c_str() << ":" << m_line << "] " << m_oss.str().c_str() << std::endl;
#endif

    }

    std::ostringstream& stream()
    {
        return m_oss;
    }

private:
    std::ostringstream m_oss;
    std::string m_file_name;
    int m_line;
};



//response info struct
//you should check timeout firstly in your cb
//then check error_msg, if it's empty, no error happened
struct ResponseInfo
{
    //true if timeout
    bool timeout;
    //not empty when error happened
    std::string error_msg;
    //raw response: headers, (chunked)content
    std::string raw_response;

    //http version string
    std::string http_version;
    //status code. when -1, it means something wrong with response's stream, maybe can not parse headers
    int status_code;
    //status code message
    std::string status_msg;

    //headers in key-value style. all keys are lowered
    std::map<std::string, std::string> headers;
    //content
    std::string content;

    ResponseInfo()
        : timeout(false)
        , status_code(-1)
    {
    }
};


//helper calss
//parse url
struct UrlParser
{
    //service name, always before "://" in url, http(default) or https,...
    std::string service;
    //host name. contains port number if has
    std::string host_all;
    //path, defalut "/"
    std::string path;
    //host name. does not contain port number. for dns using
    std::string host_part;
    //query param. usually after path in url, and separated by "?" with path
    std::string query_param;
    //port number. 0 if not specified
    unsigned short port;
    //for getaddrinfo, if no port, use service name
    std::string normalized_service;


    UrlParser()
        : port(0)
    {
    }


    void Parse(const std::string& url)
    {
        size_t service_pos = url.find("://");
        if (std::string::npos != service_pos)
        {
            service = url.substr(0, service_pos);
            boost::algorithm::to_lower(service);
        }
        if (service.empty())
        {
            service = "http";
        }

        service_pos = service_pos == std::string::npos ? 0 : service_pos + 3;
        size_t host_pos = url.find_first_of("/?", service_pos);
        if (std::string::npos == host_pos)
        {
            host_all = url.substr(service_pos);
            path = "/";
        }
        else
        {
            host_all = url.substr(service_pos, host_pos - service_pos);
            path = url.substr(host_pos);
        }

        size_t param_pos = path.find('?');
        if (std::string::npos != param_pos)
        {
            query_param = path.substr(param_pos + 1);
            path = path.substr(0, param_pos);
        }
        if (path.empty())
        {
            path = "/";
        }

        size_t port_pos = host_all.find(':');
        if (std::string::npos != port_pos)
        {
            host_part = host_all.substr(0, port_pos);
            std::string port_str = host_all.substr(port_pos + 1);
            port = static_cast<unsigned short>(strtoul(port_str.c_str(), NULL, 10));
            if (0 == port)
            {
                HTTP_CLIENT_ERROR << "port str[" << port_str << "] can not be converted to number, set port number 0";
            }
        }
        else
        {
            host_part = host_all;
        }

        if (port)//if no port, use service name
        {
            normalized_service = boost::lexical_cast<std::string>(port);
        }
        else
        {
            normalized_service = service;
        }

        HTTP_CLIENT_INFO << "url[" << url << "] parse result:\r\n"
            << "service=" << service
            << ", host_all=" << host_all
            << ", path=" << path
            << ", host_part=" << host_part
            << ", query_param=" << query_param
            << ", port=" << port
            << ", normalized_service=" << normalized_service;
    }
};



//callback function signature
typedef boost::function<void(const ResponseInfo& r, void *client_data, void *request_data)> HttpClientCallback;



enum HTTP_METHOD
{
    METHOD_UNKNOWN,
    METHOD_POST,
    METHOD_GET,
    METHOD_PUT,
    METHOD_DELETE,
    METHOD_HEAD,
};


//key point: async http client class
//ps: one instance can only make one request in its lifetime
//always HTTP/1.1
class CAsyncHttpClient
{
public:
    //************************************
    // brief:    constructor
    // name:     CAsyncHttpClient::CAsyncHttpClient
    // param:    boost::asio::io_service & io_service
    // param:    const unsigned short timeout           timeout setting, seconds
    // param:    void * client_data                     user-defined extra data pointer
    // param:    const bool throw_in_cb                 if true, throw when exception in cb, otherwise, no-throw
    // return:   
    // ps:      
    //************************************
    CAsyncHttpClient(boost::asio::io_service& io_service,
        const unsigned short timeout, void *client_data,
        const bool throw_in_cb = false)
        : m_io_service(io_service)
        , m_timeout(timeout)
        , m_client_data(client_data)
        , m_deadline_timer(io_service)
        , m_cb_called(false)
        , m_throw_in_cb(throw_in_cb)
        , m_method(METHOD_UNKNOWN)
        , m_sock(m_io_service)
    {
    }

    //************************************
    // brief:    destructor
    // name:
    // return:
    // ps:       if have not called cb, call cb with error "abandoned"
    //************************************
    ~CAsyncHttpClient()
    {
        if (m_response.error_msg.empty())
        {
            m_response.error_msg = "abandoned";
        }
        DoCallback();
    }


    //************************************
    // brief:    make request
    // name:     CAsyncHttpClient::makePost
    // param:    HttpClientCallback cb                                  callback function
    // param:    const std::string & url                                request url
    // param:    const std::map<std::string, std::string> & headers     headers info of key-value style
    // param:    const std::string & query_param                        query param. usually appended to url after "?"
    // param:    std::string & body                                     post data
    // param:    void * request_data                                    user-defined extra data pointer
    // return:   void
    // ps:       no return value, if error, it will throw
    //************************************
    void makePost(HttpClientCallback cb, const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& query_param,
        const std::string& body, void *request_data)
    {
        makeRequest(cb, METHOD_POST, url, headers, query_param, body, request_data);
    }

    //see above
    void makeGet(HttpClientCallback cb, const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& query_param, void *request_data)
    {
        makeRequest(cb, METHOD_GET, url, headers, query_param, "", request_data);
    }

    //see above
    void makePut(HttpClientCallback cb, const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& query_param,
        const std::string& body, void *request_data)
    {
        makeRequest(cb, METHOD_PUT, url, headers, query_param, body, request_data);
    }

    //see above
    void makeDelete(HttpClientCallback cb, const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& query_param,
        const std::string& body, void *request_data)
    {
        makeRequest(cb, METHOD_DELETE, url, headers, query_param, body, request_data);
    }


    //************************************
    // brief:    static tool function, transform key-value style param to string style param with given separator
    // name:     CAsyncHttpClient::build_kv_string
    // param:    const std::map<std::string, std::string> & kv_param
    // param:    const std::string & kv_sep                             separator between key and value
    // param:    const std::string & pair_sep                           separator between key-value pairs
    // return:   std::string
    // ps:       
    //************************************
    static std::string build_kv_string(const std::map<std::string, std::string>& kv_param,
        const std::string& kv_sep = "=", const std::string& pair_sep = "&")
    {
        std::string s;
        for (std::map<std::string, std::string>::const_iterator iterKey = kv_param.begin();
            iterKey != kv_param.end();
            ++iterKey)
        {
            s += iterKey->first + kv_sep + iterKey->second + pair_sep;
        }
        boost::algorithm::erase_last(s, "&");
        return s;
    }

    //************************************
    // brief:    static tool function, transform string style param to key-value style param using given separator, see above
    // name:     CAsyncHttpClient::parse_kv_string
    // param:    const std::string & s
    // param:    std::map<std::string, std::string> & kv_param
    // param:    const std::string & kv_sep
    // param:    const std::string & pair_sep
    // return:   void
    // ps:       
    //************************************
    static void parse_kv_string(const std::string& s, std::map<std::string, std::string>& kv_param,
        const std::string& kv_sep = "=", const std::string& pair_sep = "&")
    {
        kv_param.clear();

        std::vector<std::string> pairs;
        boost::algorithm::split(pairs, s, boost::algorithm::is_any_of(pair_sep));
        for (std::vector<std::string>::iterator iterStr = pairs.begin();
            iterStr != pairs.end();
            ++iterStr)
        {
            if (iterStr->empty())
            {
                HTTP_CLIENT_WARN << "encountered an empty pair";
                continue;
            }
            std::vector<std::string> kv;
            boost::algorithm::split(kv, *iterStr, boost::algorithm::is_any_of(kv_sep));
            if (2 != kv.size())
            {
                HTTP_CLIENT_WARN << "encountered a pair[" << *iterStr << "] which can not split 2 parts by [" << kv_sep << "]";
                continue;
            }
            kv_param[kv[0]] = kv[1];
        }
    }


    //************************************
    // brief:    static tool function, url encode/decode
    // name:     CAsyncHttpClient::url_encode
    // param:    const std::string & s
    // return:   std::string
    // ps:       
    //************************************
    static std::string url_encode(const std::string& s)
    {
        std::string strEncoded;
        for (std::string::const_iterator iterCh = s.begin(); iterCh != s.end(); ++iterCh)
        {
            const unsigned char ch = *iterCh;
            if (isalnum(ch) || (ch == '-') || (ch == '_') || (ch == '.') || (ch == '~'))
            {
                strEncoded += ch;
            }
            else if (ch == ' ')
            {
                strEncoded += '+';
            }
            else
            {
                strEncoded += '%';
                strEncoded += tohex(ch >> 4);//高4位
                strEncoded += tohex(ch % 16);//低4位
            }
        }
        return strEncoded;
    }

    //see above
    static std::string url_decode(const std::string& s)
    {
        std::string strDecoded;
        for (std::string::const_iterator iterCh = s.begin(); iterCh != s.end();)
        {
            if (*iterCh == '+')
            {
                strDecoded += ' ';
                ++iterCh;
            }
            else if (*iterCh == '%')
            {
                if (++iterCh == s.end())
                {
                    break;
                }
                unsigned char high = fromhex(*iterCh);

                if (++iterCh == s.end())
                {
                    break;
                }
                unsigned char low = fromhex(*iterCh);

                strDecoded += (high * 16 + low);

                ++iterCh;
            }
            else
            {
                strDecoded += *iterCh;
                ++iterCh;
            }
        }
        return strDecoded;
    }


private:
    void makeRequest(HttpClientCallback cb,
        const HTTP_METHOD m, const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& query_param,
        const std::string& body, void *request_data)
    {
        m_cb = cb;
        m_request_data = request_data;
        m_method = m;
        m_urlparser.Parse(url);

        //construct complete query_param
        std::string query_param_all;
        if (!m_urlparser.query_param.empty() && !query_param.empty())
        {
            query_param_all = m_urlparser.query_param + "&" + query_param;
        }
        else
        {
            query_param_all = m_urlparser.query_param + query_param;
        }

        m_request_string = build_request_string(m_urlparser.service,
            m_urlparser.host_all, m_urlparser.path, query_param_all,
            m_method, headers, body);
        HTTP_CLIENT_INFO << "request_string:\r\n" << m_request_string;

        //start timeout
        m_deadline_timer.expires_from_now(boost::posix_time::seconds(m_timeout));
        m_deadline_timer.async_wait(boost::bind(&CAsyncHttpClient::timeout_cb, this,
            boost::asio::placeholders::error));
        boost::asio::spawn(m_io_service, boost::bind(&CAsyncHttpClient::yield_func, this, _1));
    }

    void yield_func(boost::asio::yield_context yield)
    {
        do 
        {
            boost::system::error_code ec;

            boost::asio::ip::tcp::resolver rs(m_io_service);

            std::string query_host(m_urlparser.host_part);
            std::string query_serivce(m_urlparser.normalized_service);
            boost::asio::ip::tcp::resolver::query q(query_host, query_serivce);
            boost::asio::ip::tcp::resolver::iterator ep_iter = rs.async_resolve(q, yield[ec]);
            if (ec)
            {
                m_response.error_msg = "can not resolve addr which has host=";
                m_response.error_msg += query_host + " and service="
                    + query_serivce + ", error:" + ec.message();
                HTTP_CLIENT_ERROR << m_response.error_msg;
                break;
            }

            boost::asio::async_connect(m_sock, ep_iter, yield[ec]);
            if (ec)
            {
                m_response.error_msg = "can not connect to addr which has host=";
                m_response.error_msg += query_host + " and service=" + query_serivce + ", error:" + ec.message();
                HTTP_CLIENT_ERROR << m_response.error_msg;
                break;
            }

            boost::asio::async_write(m_sock, boost::asio::buffer(m_request_string), yield[ec]);
            if (ec)
            {
                m_response.error_msg = "can not send data to addr which has host=";
                m_response.error_msg += query_host + " and service=" + query_serivce + ", error:" + ec.message();
                HTTP_CLIENT_ERROR << m_response.error_msg;
                break;
            }

            std::string content_when_header;
            {
                /*
                see http://www.boost.org/doc/libs/1_58_0/doc/html/boost_asio/reference/async_read_until/overload1.html
                After a successful async_read_until operation,
                the streambuf may contain additional data beyond the delimiter.
                An application will typically leave that data in the streambuf
                for a subsequent async_read_until operation to examine.
                */
                boost::asio::streambuf response_buf;
                boost::asio::async_read_until(m_sock, response_buf, "\r\n\r\n", yield[ec]);
                if (ec)
                {
                    m_response.error_msg = "can not recv response header, error:" + ec.message();
                    HTTP_CLIENT_ERROR << m_response.error_msg;
                    break;
                }

                std::stringstream ss;
                ss << &response_buf;
                std::string headers_contained = ss.str();
                size_t headers_pos = headers_contained.find("\r\n\r\n");
                assert(headers_pos != std::string::npos);
                std::string headers_exactly = headers_contained.substr(0, headers_pos + 4);
                content_when_header = headers_contained.substr(headers_pos + 4);

                feed_response(headers_exactly, "");
                HTTP_CLIENT_INFO << "response headers:\r\n" << m_response.raw_response.c_str();
                if (!parse_response_headers(m_response.raw_response, m_response))
                {
                    m_response.error_msg = "can not parse response header, invalid header, header:\r\n"
                        + m_response.raw_response;
                    HTTP_CLIENT_ERROR << m_response.error_msg;
                    break;
                }
            }

            /*
            see http://www.w3.org/Protocols/rfc2616/rfc2616-sec4.html#sec4.4
            1. check status code if 1xx, 204, and 304, check request methon if HEAD, no content
            2. check if contained Transfer-Encoding, and chunked, recv chunked content
                see https://zh.wikipedia.org/zh-cn/分块传输编码
            3. check if contained Transfer-Encoding, and no chunked, recv content until eof
            4. check if contained Content-Length, recv content with exactly length
            5. else, recv until eof
            ps: do not consider "multipart/byteranges"
            */

            if ((0 == (m_response.status_code / 200) && 1 == (m_response.status_code / 100))
                || 204 == m_response.status_code
                || 304 == m_response.status_code
                || METHOD_HEAD == m_method)
            {
                HTTP_CLIENT_INFO <<"no content";
            }
            else if (m_response.headers.count("transfer-encoding")
                && boost::algorithm::icontains(m_response.headers["transfer-encoding"], "chunked"))
            {
                HTTP_CLIENT_INFO << "chunked content";

                std::string chunk_content;
                if (!content_when_header.empty())
                {
                    std::string content;
                    if (reach_chunk_end(content_when_header, chunk_content, content))
                    {
                        feed_response(chunk_content, content);
                        break;
                    }
                }

                while (true)
                {
                    boost::asio::streambuf response_buf;
                    boost::asio::async_read(m_sock, response_buf,
                        boost::asio::transfer_at_least(1),
                        yield[ec]);
                    if (ec)
                    {
                        if (ec != boost::asio::error::eof)
                        {
                            m_response.error_msg = ec.message();
                        }
                        break;
                    }

                    std::string content;
                    if (reach_chunk_end(response_buf, chunk_content, content))
                    {
                        feed_response(chunk_content, content);
                        break;
                    }
                }
            }
            else if (0 == m_response.headers.count("transfer-encoding") && m_response.headers.count("content-length"))
            {
                HTTP_CLIENT_INFO << "content with content-length";

                feed_response(content_when_header, content_when_header);
                size_t content_length = boost::lexical_cast<size_t>(m_response.headers["content-length"]);
                if (content_when_header.size() < content_length)
                {
                    boost::asio::streambuf response_buf;
                    boost::asio::async_read(m_sock, response_buf,
                        boost::asio::transfer_at_least(content_length - content_when_header.size()),
                        yield[ec]);
                    if (ec)
                    {
                        m_response.error_msg = ec.message();
                        break;
                    }
                    feed_response(response_buf, true);
                }
            }
            else
            {
                HTTP_CLIENT_INFO << "recv content till closed";

                feed_response(content_when_header, content_when_header);
                while (true)
                {
                    boost::asio::streambuf response_buf;
                    boost::asio::async_read(m_sock, response_buf,
                        boost::asio::transfer_at_least(1),
                        yield[ec]);
                    if (ec)
                    {
                        if (ec != boost::asio::error::eof)
                        {
                            m_response.error_msg = ec.message();
                        }
                        break;
                    }
                    feed_response(response_buf, true);
                }
            }

            HTTP_CLIENT_INFO << "response content:\r\n" << m_response.content;

        } while (false);

        DoCallback();
    }


    static unsigned char tohex(const unsigned char x)
    {
        return  (x > 9) ? (x - 10 + 'A') : (x + '0');
    }

    static unsigned char fromhex(const unsigned char x) 
    { 
        unsigned char y = 0;
        if (x >= 'A' && x <= 'Z')
        {
            y = x - 'A' + 10;
        }
        else if (x >= 'a' && x <= 'z')
        {
            y = x - 'a' + 10;
        }
        else if (x >= '0' && x <= '9')
        {
            y = x - '0';
        }
        else
        {
            y = 0;
        }
        return y;
    }


    //always HTTP/1.1
    static std::string build_request_string(const std::string service_name, const std::string host,
        const std::string& path, const std::string& query_param,
        const HTTP_METHOD m, const std::map<std::string, std::string>& headers,
        const std::string& body)
    {
        std::stringstream ss;

        switch (m)
        {
        case METHOD_POST:
            ss << "POST ";
            break;

        case METHOD_GET:
            ss << "GET ";
            break;

        case METHOD_PUT:
            ss << "PUT ";
            break;

        case METHOD_DELETE:
            ss << "DELETE ";
            break;

        default:
            break;
        }

        if (!service_name.empty())
        {
            ss << service_name << "://";
        }
        ss << host << path;
        if (!query_param.empty())
        {
            ss << "?" << query_param;
        }
        ss << " HTTP/1.1" << "\r\n";

        for (std::map<std::string, std::string>::const_iterator iterKey = headers.begin();
            iterKey != headers.end();
            ++iterKey)
        {
            ss << iterKey->first << ": " << iterKey->second << "\r\n";
        }
        if (0 == headers.count("Host"))
        {
            ss << "Host: " << host << "\r\n";
        }
        if (0 == headers.count("Content-Length"))
        {
            ss << "Content-Length: " << body.size() << "\r\n";
        }

        ss << "\r\n";
        ss << body;

        return ss.str();
    }


    static bool parse_response_headers(const std::string& s, ResponseInfo& r)
    {
        bool bReturn = false;
        do 
        {
            std::stringstream ss(s);
            ss >> r.http_version;
            ss >> r.status_code;
            std::getline(ss, r.status_msg);
            boost::algorithm::trim(r.status_msg);

            if (!ss)
            {
                HTTP_CLIENT_ERROR << "can not get status line";
                break;
            }

            while (ss)
            {
                std::string per_line;
                std::getline(ss, per_line);
                size_t pos = per_line.find(':');
                if (std::string::npos == pos)
                {
                    continue;
                }
                std::string key = per_line.substr(0, pos);
                boost::algorithm::trim(key);
                if (key.empty())
                {
                    HTTP_CLIENT_WARN << "encountered an empty key";
                    continue;
                }
                boost::algorithm::to_lower(key);
                std::string value = per_line.substr(pos + 1);
                boost::algorithm::trim(value);
                r.headers[key] = value;
            }

            bReturn = true;

        } while (false);

        return bReturn;
    }

    //************************************
    // brief:    check if reached ending chunk
    // name:     CHttpClient::reach_chunk_end
    // param:    std::string & cur_chunk        current chunk
    // param:    std::string & all_chunk        cur_chunk will be appended to all_chunk, using all_chunk to check
    // param:    std::string & content          if ending, contains all content parsed from all_chunk, otherwise not-defined
    // return:   bool
    // ps:
    //************************************
    static bool reach_chunk_end(const std::string& cur_chunk, std::string& all_chunk, std::string& content)
    {
        HTTP_CLIENT_INFO << "response chunk:\r\n" << cur_chunk;

        all_chunk += cur_chunk;
        content.clear();

        bool reach_end = false;
        size_t pos = 0;
        while (true)
        {
            //next \r\n
            size_t next_pos = all_chunk.find("\r\n", pos);
            if (std::string::npos == next_pos)
            {
                break;
            }
            std::string chunk_size_str = all_chunk.substr(pos, next_pos - pos);
            boost::algorithm::trim(chunk_size_str);
            if (chunk_size_str.empty())
            {
                pos = next_pos + 2;
                continue;
            }

            //chunk size
            unsigned long chunk_size = strtoul(chunk_size_str.c_str(), NULL, 16);
            if (0 == chunk_size)
            {
                reach_end = true;
                break;
            }

            content += all_chunk.substr(next_pos + 2, chunk_size);
            pos = next_pos + 2 + chunk_size + 2;//\r\n before and after
        }

        return reach_end;
    }

    //cur_buf can not be const, or you will get an address???
    static bool reach_chunk_end(boost::asio::streambuf& cur_buf, std::string& all_chunk, std::string& content)
    {
        std::stringstream ss;
        ss << &cur_buf;
        std::string cur_chunk = ss.str();
        return reach_chunk_end(cur_chunk, all_chunk, content);
    }


    void feed_response(const std::string& raw, const std::string& content)
    {
        m_response.raw_response += raw;
        m_response.content += content;
    }

    //cur_buf can not be const, or you will get an address???
    void feed_response(boost::asio::streambuf& buf, const bool is_content = false)
    {
        std::stringstream ss;
        ss << &buf;
        std::string s = ss.str();
        if (is_content)
        {
            feed_response(s, s);
        }
        else
        {
            feed_response(s, "");
        }
    }


    void timeout_cb(const boost::system::error_code &ec)
    {
        if (ec)
        {
            if (ec != boost::asio::error::operation_aborted)
            {
                m_response.error_msg = "timeout callback encountered an error:" + ec.message();
                HTTP_CLIENT_ERROR << m_response.error_msg;
                DoCallback();
            }
            else
            {
                HTTP_CLIENT_INFO << "timeout callback was canceled";
            }
        }
        else
        {
            HTTP_CLIENT_ERROR << "timeout";
            m_response.timeout = true;
            m_response.error_msg = "timeout";
            boost::system::error_code ec_close;
            m_sock.close(ec_close);
            DoCallback();
        }
    }

    void DoCallback()
    {
        if (!m_cb_called)
        {
            boost::system::error_code ec_cancel;
            m_deadline_timer.cancel(ec_cancel);

            m_cb_called = true;
            try
            {
                m_cb(m_response, m_client_data, m_request_data);
            }
            catch (...)
            {
                HTTP_CLIENT_ERROR << "exception happened in callback function";
                if (m_throw_in_cb)
                {
                    HTTP_CLIENT_INFO << "throw";
                    throw;
                }
            }
        }
    }


private:
    boost::asio::io_service& m_io_service;
    const unsigned short m_timeout;
    void *m_client_data;
    HttpClientCallback m_cb;
    void *m_request_data;
    boost::asio::deadline_timer m_deadline_timer;
    bool m_cb_called;
    bool m_throw_in_cb;
    HTTP_METHOD m_method;
    UrlParser m_urlparser;

    boost::asio::ip::tcp::socket m_sock;
    std::string m_request_string;

    ResponseInfo m_response;
};


