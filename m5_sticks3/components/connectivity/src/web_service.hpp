#pragma once
#include "connectivity_types.hpp"
#include "esp_http_server.h"
#include "motion_stream.hpp"
#include "web_policy.hpp"
#include <array>
#include <atomic>
namespace connectivity
{
// HTTP task owns all sessions and sockets. Owner task only schedules bounded work.
class WebService
{
  public:
    static WebService *create(httpd_handle_t server, RequestHandler handler, void *context);
    static void destroy(WebService *self);
    esp_err_t start();
    void process();

  private:
    WebService(httpd_handle_t server, RequestHandler handler, void *context);
    struct Session
    {
        char token[33]{};
        int64_t last_used = 0;
        uint32_t generation = 0;
        web::ReplayWindow replay{};
    };
    struct Batch
    {
        std::array<model::StreamSample, 5> samples{};
        size_t count = 0;
    };
    struct Client
    {
        WebService *owner = nullptr;
        int fd = -1, session = -1;
        uint32_t generation = 0, after = 0, lost = 0;
        bool subscribed = false;
        int64_t opened = 0, last_progress = 0;
        web::DropQueue<Batch, 4> queue{};
        char wire[1028]{};
        size_t size = 0, sent = 0;
        char control[128]{};
        uint8_t control_type = 1;
        size_t control_size = 0;
        bool control_pending = false;
        char pong[125]{};
        size_t pong_size = 0;
        bool pong_pending = false, closing = false;
    };
    static esp_err_t route(httpd_req_t *request);
    static esp_err_t preflight(httpd_req_t *request);
    static esp_err_t stream_route(httpd_req_t *request);
    static void work(void *context);
    static void closed(void *context);
    esp_err_t handle(httpd_req_t *request);
    esp_err_t socket(httpd_req_t *request);
    void tick();
    void collect(Client &client);
    void send(Client &client);
    bool active(int index, uint32_t generation) const;
    int authenticate(const char *token, bool touch);
    int bearer(httpd_req_t *request);
    bool same_origin(httpd_req_t *request, bool required);
    void frame(Client &client, const char *data, size_t size, uint8_t type = 1);
    httpd_handle_t server_;
    RequestHandler handler_;
    void *context_;
    std::atomic<bool> pending_{false};
    int64_t next_work_ = 0, next_batch_ = 0, next_status_ = 0;
    uint32_t boot_ = 0, generation_ = 0, session_sequence_ = 0;
    std::array<Session, 2> sessions_{};
    std::array<Client, 2> clients_{};
};
} // namespace connectivity
