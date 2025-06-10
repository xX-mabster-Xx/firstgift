#pragma once
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>
#include <td/telegram/td_api.hpp>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <atomic>
#include <mutex>

namespace td_api = td::td_api;

enum class AgentState
{
    Active,
    Inactive,
    Gifted,
    Testing
};

class TdInterface
{
public:
    TdInterface(int32_t api_id, const std::string &api_hash);
    void loop();
    void set_on_authorized_callback(std::function<void()> callback)
    {
        on_authorized_callback_ = std::move(callback);
    }
    void test();
    void check_for_upgrade();
    void upgrade_loop(int);
    std::atomic<bool> checking{false};
    std::atomic<int> sent_{0};
    std::atomic<int> received_{0};

private:
    std::unordered_map<int, std::chrono::_V2::steady_clock::time_point> times;
    std::mutex times_mutex_;
    std::string bb = "";
    std::string id = "689019";
    using Object = td::td_api::object_ptr<td::td_api::Object>;
    void on_authorized();
    std::function<void()> on_authorized_callback_;
    std::unique_ptr<td::ClientManager> client_manager_;
    std::int32_t client_id_;
    std::uint64_t current_query_id_ = 0;
    std::uint64_t authentication_query_id_ = 0;
    int32_t api_id_;
    std::string api_hash_;
    td::td_api::object_ptr<td::td_api::AuthorizationState> authorization_state_;
    bool are_authorized_ = false;
    bool need_restart_ = false;
    std::map<std::uint64_t, std::function<void(Object)>> handlers_;
    void restart();
    std::uint64_t next_query_id();
    void send_query(td::td_api::object_ptr<td::td_api::Function> f, std::function<void(Object)> = {});
    void send_query_check();
    void send_query_upgrade();
    void process_response(td::ClientManager::Response response);
    void process_update(td::td_api::object_ptr<td::td_api::Object> update);
    void on_authorization_state_update();
    void check_authentication_error(Object object);
    std::function<void(TdInterface::Object)> create_authentication_query_handler();
    std::string get_user_name(std::int64_t user_id) const;
    std::string get_chat_title(std::int64_t chat_id) const;
};
