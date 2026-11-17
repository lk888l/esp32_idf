#include "web_service.hpp"
#include "cJSON.h"
#include "connectivity_policy.hpp"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/sockets.h"
#include "motion_state.hpp"
#include "web_pairing.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
namespace connectivity
{
namespace
{
extern "C"
{
    extern const char web_html_start[] asm("_binary_index_html_start");
    extern const char web_html_end[] asm("_binary_index_html_end");
    extern const char web_js_start[] asm("_binary_app_js_start");
    extern const char web_js_end[] asm("_binary_app_js_end");
}
constexpr int64_t idle_us = 30LL * 60 * 1000000;
esp_err_t json(httpd_req_t *r, const char *body, const char *status = "200 OK")
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(r, body, HTTPD_RESP_USE_STRLEN);
}
esp_err_t error(httpd_req_t *r, const char *why, const char *status = "400 Bad Request")
{
    char text[128];
    snprintf(text, sizeof(text), "{\"ok\":false,\"error\":\"%s\"}", why);
    json(r, text, status);
    return ESP_FAIL; // Never drain an unbounded hostile body.
}
bool read_body(httpd_req_t *r, char (&body)[257])
{
    char type[64]{};
    if (r->content_len <= 0 || r->content_len > 256 ||
        httpd_req_get_hdr_value_str(r, "Content-Type", type, sizeof(type)) != ESP_OK ||
        (strcmp(type, "application/json") && strcmp(type, "application/json; charset=utf-8")))
        return false;
    size_t at = 0;
    const int64_t until = esp_timer_get_time() + 2'000'000;
    while (at < r->content_len)
    {
        if (esp_timer_get_time() >= until)
            return false;
        int n = httpd_req_recv(r, body + at, r->content_len - at);
        if (n <= 0)
            return false;
        at += n;
    }
    body[at] = 0;
    return !memchr(body, 0, at) && valid_utf8({body, at});
}
// Flat objects only on the session/stream control interface. Reject duplicate
// keys and escaped NUL before parsing (cJSON strings are NUL terminated).
cJSON *parse(const char *body)
{
    if (strstr(body, "\\u0000"))
        return nullptr;
    bool quoted = false;
    for (const char *p = body; *p; ++p)
    {
        if (quoted && *p == '\\' && p[1])
        {
            ++p;
            continue;
        }
        if (*p == '"')
            quoted = !quoted;
        if (!quoted && (*p == '[' || (*p == '{' && p != body)))
            return nullptr;
    }
    const char *end = nullptr;
    auto *root = cJSON_ParseWithOpts(body, &end, true);
    if (!cJSON_IsObject(root))
    {
        cJSON_Delete(root);
        return nullptr;
    }
    for (auto *a = root->child; a; a = a->next)
        for (auto *b = a->next; b; b = b->next)
            if (!strcmp(a->string, b->string))
            {
                cJSON_Delete(root);
                return nullptr;
            }
    return root;
}
const char *string(cJSON *root, const char *key)
{
    auto *v = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
void random_token(char *out)
{
    uint8_t bytes[16];
    esp_fill_random(bytes, sizeof(bytes));
    for (size_t i = 0; i < 16; ++i)
        snprintf(out + i * 2, 3, "%02x", bytes[i]);
}
} // namespace
WebService::WebService(httpd_handle_t s, RequestHandler h, void *c) : server_(s), handler_(h), context_(c)
{
    boot_ = esp_random();
    generation_ = web_session_generation();
    for (auto &client : clients_)
        client.owner = this;
}
WebService *WebService::create(httpd_handle_t s, RequestHandler h, void *c)
{
    void *memory = heap_caps_malloc(sizeof(WebService), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory ? new (memory) WebService(s, h, c) : nullptr;
}
void WebService::destroy(WebService *self)
{
    if (!self)
        return;
    model::MotionStream::instance().request(false);
    self->~WebService();
    heap_caps_free(self);
}
esp_err_t WebService::start()
{
    for (const auto *uri :
         {"/", "/app.js", "/api/v1/web", "/api/v1/session", "/api/v1/control", "/api/v1/stream"})
    {
        httpd_uri_t route{};
        route.uri = uri;
        route.user_ctx = this;
        route.handler = WebService::route;
        route.method =
            (!strcmp(uri, "/api/v1/session") || !strcmp(uri, "/api/v1/control")) ? HTTP_POST : HTTP_GET;
        route.is_websocket = !strcmp(uri, "/api/v1/stream");
        if (route.is_websocket)
            route.handler = stream_route;
        route.handle_ws_control_frames = true;
        route.ws_pre_handshake_cb = route.is_websocket ? preflight : nullptr;
        const auto result = httpd_register_uri_handler(server_, &route);
        if (result != ESP_OK)
            return result;
    }
    return ESP_OK;
}
void WebService::process()
{
    const auto now = esp_timer_get_time();
    if (now < next_work_)
        return;
    next_work_ = now + 10'000;
    if (!pending_.exchange(true))
        if (httpd_queue_work(server_, work, this) != ESP_OK)
            pending_.store(false);
}
void WebService::work(void *context)
{
    auto *self = static_cast<WebService *>(context);
    self->tick();
    self->pending_.store(false);
}
esp_err_t WebService::preflight(httpd_req_t *r)
{
    return static_cast<WebService *>(r->user_ctx)->socket(r);
}
esp_err_t WebService::stream_route(httpd_req_t *r)
{
    if (r->method == HTTP_GET)
        return r->sess_ctx ? ESP_OK : error(r, "websocket_required");
    return static_cast<WebService *>(r->user_ctx)->socket(r);
}
esp_err_t WebService::route(httpd_req_t *r)
{
    return static_cast<WebService *>(r->user_ctx)->handle(r);
}
bool WebService::same_origin(httpd_req_t *r, bool required)
{
    char origin[128]{}, host[96]{};
    if (!httpd_req_get_hdr_value_len(r, "Origin"))
        return !required;
    if (httpd_req_get_hdr_value_str(r, "Origin", origin, sizeof(origin)) != ESP_OK ||
        httpd_req_get_hdr_value_str(r, "Host", host, sizeof(host)) != ESP_OK)
        return false;
    char expected[128];
    snprintf(expected, sizeof(expected), "http://%s", host);
    // Only numeric device addresses are supported; reject
    // DNS-rebinding hostnames by requiring a local interface address.
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    char ip[48]{};
    if (getsockname(httpd_req_to_sockfd(r), reinterpret_cast<sockaddr *>(&address), &length) != 0)
        return false;
    if (address.ss_family == AF_INET)
    {
        inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in *>(&address)->sin_addr, ip, sizeof(ip));
    }
    else if (address.ss_family == AF_INET6)
    {
        const auto &ipv6 = reinterpret_cast<sockaddr_in6 *>(&address)->sin6_addr;
        if (!IN6_IS_ADDR_V4MAPPED(&ipv6))
            return false;
        inet_ntop(AF_INET, reinterpret_cast<const uint8_t *>(&ipv6) + 12, ip, sizeof(ip));
    }
    else
        return false;
    char with_port[64];
    snprintf(with_port, sizeof(with_port), "%s:80", ip);
    return (!strcmp(host, ip) || !strcmp(host, with_port)) && !strcmp(origin, expected);
}
bool WebService::active(int i, uint32_t generation) const
{
    return i >= 0 && i < int(sessions_.size()) && sessions_[i].token[0] &&
           sessions_[i].generation == generation && generation_ == web_session_generation() &&
           esp_timer_get_time() - sessions_[i].last_used < idle_us;
}
int WebService::authenticate(const char *token, bool touch)
{
    for (size_t i = 0; i < sessions_.size(); ++i)
        if (active(i, sessions_[i].generation) && token[0] && constant_time_equal(token, sessions_[i].token))
        {
            if (touch)
                sessions_[i].last_used = esp_timer_get_time();
            return int(i);
        }
    return -1;
}
int WebService::bearer(httpd_req_t *r)
{
    char value[48]{};
    if (httpd_req_get_hdr_value_str(r, "Authorization", value, sizeof(value)) != ESP_OK ||
        strncmp(value, "Bearer ", 7))
        return -1;
    return authenticate(value + 7, true);
}
esp_err_t WebService::handle(httpd_req_t *r)
{
    if (generation_ != web_session_generation())
    {
        for (auto &s : sessions_)
        {
            s.~Session();
            new (&s) Session();
        }
        generation_ = web_session_generation();
    }
    if (!same_origin(r, false))
        return error(r, "origin_rejected", "403 Forbidden");
    if (!strcmp(r->uri, "/") || !strcmp(r->uri, "/app.js"))
    {
        const bool html = !strcmp(r->uri, "/");
        httpd_resp_set_type(r, html ? "text/html; charset=utf-8" : "text/javascript; charset=utf-8");
        httpd_resp_set_hdr(r, "Cache-Control", "no-cache");
        httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
        httpd_resp_set_hdr(r, "Content-Security-Policy",
                           "default-src 'self'; script-src 'self'; style-src 'unsafe-inline'; connect-src "
                           "'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'self'");
        return httpd_resp_send(r, html ? web_html_start : web_js_start,
                               html ? web_html_end - web_html_start - 1 : web_js_end - web_js_start - 1);
    }
    if (!strcmp(r->uri, "/api/v1/web"))
    {
        char output[640];
        snprintf(output, sizeof(output),
                 "{\"v\":1,\"ok\":true,\"boot\":%lu,\"stream\":\"/api/v1/"
                 "stream\",\"rate_hz\":100,\"batch_ms\":50,\"batch_samples\":5,\"max_clients\":2,\"queued_"
                 "batches\":4,\"frame_bytes\":1024,\"fields\":[\"seq\",\"time_us\",\"roll\",\"pitch\","
                 "\"yaw\",\"valid\"],\"angle_unit\":\"deg\",\"time_source\":\"monotonic_estimate\",\"idle_"
                 "seconds\":1800,\"command_history\":32,\"transport_security\":\"trusted_lan_only\"}",
                 static_cast<unsigned long>(boot_));
        return json(r, output);
    }
    char body[257]{};
    if (!read_body(r, body))
        return error(r, "invalid_body");
    if (!strcmp(r->uri, "/api/v1/session"))
    {
        auto *root = parse(body);
        if (!root)
            return error(r, "invalid_json");
        const bool revoke = !strcmp(string(root, "op"), "revoke");
        if (revoke)
        {
            cJSON_Delete(root);
            int i = bearer(r);
            if (i < 0)
                return error(r, "unauthorized", "401 Unauthorized");
            sessions_[i].~Session();
            new (&sessions_[i]) Session();
            return json(r, "{\"ok\":true}");
        }
        auto *value = cJSON_GetObjectItemCaseSensitive(root, "code");
        const double code = cJSON_IsNumber(value) ? value->valuedouble : 0;
        const bool valid =
            std::isfinite(code) && code >= 100000 && code <= 999999 && std::floor(code) == code;
        cJSON_Delete(root);
        int slot = -1;
        for (size_t i = 0; i < sessions_.size(); ++i)
            if (!active(i, sessions_[i].generation))
            {
                slot = i;
                break;
            }
        if (slot < 0)
            return error(r, "session_limit", "429 Too Many Requests");
        if (!valid || !consume_web_pairing(uint32_t(code)))
            return error(r, "invalid_pairing", "403 Forbidden");
        auto &session = sessions_[slot];
        session.~Session();
        new (&session) Session();
        random_token(session.token);
        generation_ = web_session_generation();
        session.generation = ++session_sequence_;
        session.last_used = esp_timer_get_time();
        char output[128];
        snprintf(output, sizeof(output), "{\"ok\":true,\"session\":\"%s\",\"idle_seconds\":1800}",
                 session.token);
        return json(r, output);
    }
    const int index = bearer(r);
    if (index < 0)
        return error(r, "unauthorized", "401 Unauthorized");
    char sequence[16]{};
    if (httpd_req_get_hdr_value_str(r, "X-Command-Seq", sequence, sizeof(sequence)) != ESP_OK)
        return error(r, "invalid_sequence");
    char *end = nullptr;
    const unsigned long long number = strtoull(sequence, &end, 10);
    if (!sequence[0] || sequence[0] < '0' || sequence[0] > '9' || *end || !number || number > UINT32_MAX)
        return error(r, "invalid_sequence");
    auto &replay = sessions_[index].replay;
    const char *cached = nullptr;
    auto check = replay.check(number, body, cached);
    if (check == web::ReplayWindow::Check::replay)
        return json(r, cached);
    if (check != web::ReplayWindow::Check::fresh)
        return error(r,
                     check == web::ReplayWindow::Check::conflict ? "sequence_conflict" : "sequence_expired",
                     "409 Conflict");
    char output[kMaxResponseBytes]{};
    handler_(context_, body, strlen(body), output, sizeof(output));
    if (!output[0])
        return error(r, "empty_response", "500 Internal Server Error");
    replay.save(number, body, output);
    return json(r, output);
}
void WebService::closed(void *p)
{
    auto *client = static_cast<Client *>(p);
    auto *owner = client->owner;
    *client = Client{};
    client->owner = owner;
    bool subscribed = false;
    for (auto &c : owner->clients_)
        subscribed |= c.subscribed;
    model::MotionStream::instance().request(subscribed);
}
esp_err_t WebService::socket(httpd_req_t *r)
{
    if (r->method == HTTP_GET)
    {
        if (!same_origin(r, true))
            return error(r, "origin_rejected", "403 Forbidden");
        Client *client = nullptr;
        for (auto &c : clients_)
            if (c.fd < 0)
            {
                client = &c;
                break;
            }
        if (!client)
            return error(r, "client_limit", "503 Service Unavailable");
        client->fd = httpd_req_to_sockfd(r);
        client->opened = client->last_progress = esp_timer_get_time();
        r->sess_ctx = client;
        r->free_ctx = closed;
        return ESP_OK;
    }
    auto *client = static_cast<Client *>(r->sess_ctx);
    if (!client)
        return ESP_FAIL;
    httpd_ws_frame_t f{};
    if (httpd_ws_recv_frame(r, &f, 0) != ESP_OK || f.len > 256 || !f.final)
        return ESP_FAIL;
    char body[257]{};
    f.payload = reinterpret_cast<uint8_t *>(body);
    if (f.len && httpd_ws_recv_frame(r, &f, 256) != ESP_OK)
        return ESP_FAIL;
    if (f.type == HTTPD_WS_TYPE_CLOSE)
    {
        if (f.len > 125 || f.len == 1)
            return ESP_FAIL;
        client->subscribed = false;
        client->closing = true;
        client->queue.clear();
        client->pong_pending = false;
        memcpy(client->control, body, f.len);
        client->control_size = f.len;
        client->control_type = 8;
        client->control_pending = true;
        // IDF closes peer-initiated Close sessions during request cleanup.
        // Flush the reply here without blocking; a stalled peer is closed.
        send(*client);
        return ESP_OK;
    }
    if (f.type == HTTPD_WS_TYPE_PONG)
        return ESP_OK;
    if (f.type == HTTPD_WS_TYPE_PING)
    {
        if (f.len > 125 || client->pong_pending)
            return ESP_FAIL;
        memcpy(client->pong, body, f.len);
        client->pong_size = f.len;
        client->pong_pending = true;
        return ESP_OK;
    }
    if (f.type != HTTPD_WS_TYPE_TEXT || !f.len || memchr(body, 0, f.len) || !valid_utf8({body, f.len}))
        return ESP_FAIL;
    auto *root = parse(body);
    if (!root)
        return ESP_FAIL;
    int i = authenticate(string(root, "session"), true);
    const std::string_view op = string(root, "op");
    bool subscribe = op == "subscribe", unsubscribe = op == "unsubscribe";
    cJSON_Delete(root);
    if (i < 0 || (!subscribe && !unsubscribe) || client->control_pending)
        return ESP_FAIL;
    client->session = i;
    client->generation = sessions_[i].generation;
    client->subscribed = subscribe;
    client->after = model::MotionStream::instance().latest();
    client->queue.clear();
    snprintf(client->control, sizeof(client->control), "{\"v\":1,\"ok\":true,\"event\":\"%s\",\"boot\":%lu}",
             subscribe ? "subscribed" : "unsubscribed", static_cast<unsigned long>(boot_));
    client->control_type = 1;
    client->control_size = strlen(client->control);
    client->control_pending = true;
    bool needed = false;
    for (auto &c : clients_)
        needed |= c.subscribed;
    model::MotionStream::instance().request(needed);
    return ESP_OK;
}
void WebService::frame(Client &c, const char *data, size_t size, uint8_t type)
{
    c.wire[0] = char(0x80 | type);
    size_t header = 2;
    if (size < 126)
        c.wire[1] = char(size);
    else
    {
        c.wire[1] = 126;
        c.wire[2] = char(size >> 8);
        c.wire[3] = char(size);
        header = 4;
    }
    memcpy(c.wire + header, data, size);
    c.size = size + header;
    c.sent = 0;
}
void WebService::collect(Client &c)
{
    // Keep at most 20 unsent samples in the shared cursor as well as the bounded
    // four-batch queue. Slow readers see gaps instead of delayed history.
    auto latest = model::MotionStream::instance().latest();
    if (latest - c.after > 20)
    {
        c.lost += latest - c.after - 20;
        c.after = latest - 20;
    }
    for (unsigned i = 0; i < 4; ++i)
    {
        Batch b;
        b.count = model::MotionStream::instance().read(c.after, b.samples.data(), 5, c.lost);
        if (!b.count)
            break;
        Batch discarded;
        if (c.queue.push(b, &discarded))
            c.lost += discarded.count;
    }
}
void WebService::send(Client &c)
{
    for (unsigned budget = 0; budget < 5; ++budget)
    {
        if (!c.size)
        {
            if (c.pong_pending)
            {
                frame(c, c.pong, c.pong_size, 10);
                c.pong_pending = false;
            }
            else if (c.control_pending)
            {
                frame(c, c.control, c.control_size, c.control_type);
                c.control_pending = false;
                c.control_size = 0;
            }
            else
            {
                Batch b;
                if (!c.queue.pop(b))
                    return;
                char payload[1024];
                int at = snprintf(
                    payload, sizeof(payload),
                    "{\"v\":1,\"event\":\"motion\",\"boot\":%lu,\"lost\":%lu,\"valid\":%s,\"samples\":[",
                    static_cast<unsigned long>(boot_), static_cast<unsigned long>(c.lost),
                    model::MotionState::instance().snapshot().valid ? "true" : "false");
                for (size_t i = 0; i < b.count; ++i)
                {
                    auto &s = b.samples[i];
                    at += snprintf(payload + at, sizeof(payload) - at, "%s[%lu,%llu,%.3f,%.3f,%.3f,%s]",
                                   i ? "," : "", static_cast<unsigned long>(s.sequence),
                                   static_cast<unsigned long long>(s.time_us), double(s.roll),
                                   double(s.pitch), double(s.yaw), s.valid ? "true" : "false");
                }
                at += snprintf(payload + at, sizeof(payload) - at, "]}");
                frame(c, payload, at);
            }
        }
        const int n = ::send(c.fd, c.wire + c.sent, c.size - c.sent, MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return;
        if (n <= 0)
        {
            httpd_sess_trigger_close(server_, c.fd);
            return;
        }
        c.sent += n;
        c.last_progress = esp_timer_get_time();
        if (c.sent < c.size)
            return;
        c.size = c.sent = 0;
        if (c.closing && !c.control_pending)
            return;
    }
}
void WebService::tick()
{
    const auto now = esp_timer_get_time();
    if (generation_ != web_session_generation())
    {
        for (auto &s : sessions_)
        {
            s.~Session();
            new (&s) Session();
        }
        generation_ = web_session_generation();
    }
    const bool batch = now >= next_batch_;
    if (batch)
        next_batch_ = now + 50'000 - (now % 50'000);
    const bool status = now >= next_status_;
    if (status)
        next_status_ = now + 1'000'000;
    for (auto &s : sessions_)
        if (s.token[0] && now - s.last_used >= idle_us)
        {
            s.~Session();
            new (&s) Session();
        }
    bool needed = false;
    for (auto &c : clients_)
        if (c.fd >= 0)
        {
            if ((c.session < 0 && now - c.opened > 10'000'000) ||
                (c.session >= 0 && !active(c.session, c.generation)) ||
                (c.size && now - c.last_progress > 2'000'000))
            {
                c.subscribed = false;
                httpd_sess_trigger_close(server_, c.fd);
                continue;
            }
            needed |= c.subscribed;
            if (c.subscribed && batch)
                collect(c);
            if (status && c.session >= 0 && !c.control_pending && !c.closing)
            {
                snprintf(c.control, sizeof(c.control),
                         "{\"v\":1,\"event\":\"stream.status\",\"valid\":%s,\"heap_min\":%lu}",
                         model::MotionState::instance().snapshot().valid ? "true" : "false",
                         static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
                c.control_size = strlen(c.control);
                c.control_type = 1;
                c.control_pending = true;
            }
            send(c);
        }
    model::MotionStream::instance().request(needed);
}
} // namespace connectivity
