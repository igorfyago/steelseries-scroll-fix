// http.h : a very small HTTP server for the tuner page. Loopback only, one
// request per connection, one connection at a time.
//
// The page controls a program that filters the mouse, so the server has to
// refuse requests that a web site could make the browser send:
//   - it listens on 127.0.0.1 only
//   - Host must be this server's own address. A site that points its own name
//     at 127.0.0.1 (DNS rebinding) sends its own name there and is refused
//   - a request that carries an Origin must carry this server's origin
//   - anything that changes state needs a custom header. A page on another
//     origin cannot add one without a preflight, which is never granted
// Limits keep a stuck or hostile client from tying it up: 8 KB of headers,
// 8 KB of body, two seconds per read or write.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <string>

namespace fbw {

struct HttpRequest {
    std::string method, path, query, body;
};

struct HttpResponse {
    int         status = 200;
    std::string type = "application/json";
    std::string body;
};

using HttpHandler = HttpResponse (*)(const HttpRequest&, void* ctx);

constexpr const char* kStateHeader = "x-fly-by-wire";

class HttpServer {
public:
    bool start(uint16_t port, HttpHandler handler, void* ctx) {
        port_ = port;
        handler_ = handler;
        ctx_ = ctx;
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        listen_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_ == INVALID_SOCKET) return false;
        BOOL exclusive = TRUE;
        setsockopt(listen_, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listen_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listen_, 16) != 0) {
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
            return false;
        }
        thread_ = CreateThread(nullptr, 0, &HttpServer::entry, this, 0, nullptr);
        return thread_ != nullptr;
    }

    void stop() {
        quit_.store(true);
        if (thread_) {
            WaitForSingleObject(thread_, 3000);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (listen_ != INVALID_SOCKET) {
            closesocket(listen_);
            listen_ = INVALID_SOCKET;
        }
    }

private:
    static constexpr size_t kMaxHead = 8192;
    static constexpr size_t kMaxBody = 8192;

    static DWORD WINAPI entry(void* p) {
        static_cast<HttpServer*>(p)->loop();
        return 0;
    }

    void loop() {
        while (!quit_.load()) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(listen_, &set);
            timeval tv{0, 300000};
            if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(listen_, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            DWORD ms = 2000;
            setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
            setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
            serve(c);
            shutdown(c, SD_SEND);
            closesocket(c);
        }
    }

    static std::string lower(std::string s) {
        for (char& ch : s) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        return s;
    }

    static std::string trim(const std::string& s) {
        size_t a = 0, b = s.size();
        while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
        while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
        return s.substr(a, b - a);
    }

    // Value of a header, lower-cased name. Empty when absent.
    static std::string header(const std::string& head, const char* name) {
        size_t pos = head.find("\r\n");
        while (pos != std::string::npos) {
            const size_t start = pos + 2;
            const size_t end = head.find("\r\n", start);
            const std::string line = head.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const size_t colon = line.find(':');
            if (colon != std::string::npos && lower(trim(line.substr(0, colon))) == name) return trim(line.substr(colon + 1));
            pos = end;
        }
        return "";
    }

    void serve(SOCKET c) {
        std::string buf;
        size_t headEnd = std::string::npos;
        char chunk[2048];
        while (headEnd == std::string::npos) {
            if (buf.size() > kMaxHead) return reply(c, 431, "text/plain", "headers too large");
            const int n = recv(c, chunk, sizeof(chunk), 0);
            if (n <= 0) return;
            buf.append(chunk, static_cast<size_t>(n));
            headEnd = buf.find("\r\n\r\n");
        }
        const std::string head = buf.substr(0, headEnd + 2);
        std::string body = buf.substr(headEnd + 4);

        HttpRequest req;
        const size_t sp1 = head.find(' ');
        const size_t sp2 = sp1 == std::string::npos ? std::string::npos : head.find(' ', sp1 + 1);
        if (sp2 == std::string::npos) return reply(c, 400, "text/plain", "bad request");
        req.method = head.substr(0, sp1);
        std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
        const size_t q = target.find('?');
        if (q != std::string::npos) {
            req.query = target.substr(q + 1);
            target.resize(q);
        }
        req.path = target;

        char own1[40], own2[40];
        snprintf(own1, sizeof(own1), "127.0.0.1:%u", static_cast<unsigned>(port_));
        snprintf(own2, sizeof(own2), "localhost:%u", static_cast<unsigned>(port_));
        const std::string host = lower(header(head, "host"));
        if (host != own1 && host != own2) return reply(c, 403, "text/plain", "wrong host");

        const std::string origin = lower(header(head, "origin"));
        if (!origin.empty() && origin != std::string("http://") + own1 && origin != std::string("http://") + own2) {
            return reply(c, 403, "text/plain", "wrong origin");
        }
        if (lower(header(head, "sec-fetch-site")) == "cross-site") return reply(c, 403, "text/plain", "cross site");

        if (req.method == "POST") {
            if (header(head, kStateHeader) != "1") return reply(c, 403, "text/plain", "missing header");
            const std::string lenText = header(head, "content-length");
            const size_t len = lenText.empty() ? 0 : static_cast<size_t>(strtoul(lenText.c_str(), nullptr, 10));
            if (len > kMaxBody) return reply(c, 413, "text/plain", "body too large");
            while (body.size() < len) {
                const int n = recv(c, chunk, sizeof(chunk), 0);
                if (n <= 0) return;
                body.append(chunk, static_cast<size_t>(n));
            }
            body.resize(len);
            req.body = body;
        } else if (req.method != "GET") {
            return reply(c, 405, "text/plain", "method not allowed");
        }

        const HttpResponse res = handler_(req, ctx_);
        reply(c, res.status, res.type.c_str(), res.body);
    }

    void reply(SOCKET c, int status, const char* type, const std::string& body) {
        const char* text = status == 200 ? "OK" : status == 400 ? "Bad Request" : status == 403 ? "Forbidden"
                         : status == 404 ? "Not Found" : status == 405 ? "Method Not Allowed"
                         : status == 413 ? "Payload Too Large" : status == 431 ? "Request Header Fields Too Large"
                         : "Error";
        char head[700];
        const bool html = std::string(type).rfind("text/html", 0) == 0;
        const int n = snprintf(head, sizeof(head),
            "HTTP/1.1 %d %s\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "Cache-Control: no-store\r\n"
            "X-Content-Type-Options: nosniff\r\n"
            "X-Frame-Options: DENY\r\n"
            "%s"
            "Connection: close\r\n\r\n",
            status, text, type, body.size(),
            html ? "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; "
                   "connect-src 'self'; img-src data:; frame-ancestors 'none'; base-uri 'none'; form-action 'none'\r\n"
                 : "");
        sendAll(c, head, static_cast<size_t>(n));
        sendAll(c, body.data(), body.size());
    }

    static void sendAll(SOCKET c, const char* data, size_t len) {
        while (len > 0) {
            const int n = send(c, data, static_cast<int>(len > 65536 ? 65536 : len), 0);
            if (n <= 0) return;
            data += n;
            len -= static_cast<size_t>(n);
        }
    }

    uint16_t    port_ = 0;
    HttpHandler handler_ = nullptr;
    void*       ctx_ = nullptr;
    SOCKET      listen_ = INVALID_SOCKET;
    HANDLE      thread_ = nullptr;
    std::atomic<bool> quit_{false};
};

}  // namespace fbw
