#include "td_interface.hpp"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iostream>
#include <sstream>
#include <codecvt>
#include <locale>
#include <chrono>


std::u16string utf8_to_utf16(const std::string &utf8)
{
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
    return convert.from_bytes(utf8);
}

std::string utf16_to_utf8(const std::u16string &utf16)
{
    std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t> convert;
    return convert.to_bytes(utf16);
}

std::string utf16_safe_substring(const std::string &text_utf8, size_t utf16_offset, size_t utf16_length)
{
    auto utf16 = utf8_to_utf16(text_utf8);
    auto part = utf16.substr(utf16_offset, utf16_length);
    return utf16_to_utf8(part);
}

namespace detail
{
    template <class... Fs>
    struct overload;

    template <class F>
    struct overload<F> : public F
    {
        explicit overload(F f) : F(f)
        {
        }
    };
    template <class F, class... Fs>
    struct overload<F, Fs...>
        : public overload<F>, public overload<Fs...>
    {
        overload(F f, Fs... fs) : overload<F>(f), overload<Fs...>(fs...)
        {
        }
        using overload<F>::operator();
        using overload<Fs...>::operator();
    };
} // namespace detail

template <class... F>
auto overloaded(F... f)
{
    return detail::overload<F...>(f...);
}

namespace td_api = td::td_api;

using td_api::make_object;
using td_api::object_ptr;

TdInterface::TdInterface(int32_t api_id, const std::string &api_hash)
    : api_id_(api_id), api_hash_(api_hash)
{
    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(1));
    client_manager_ = std::make_unique<td::ClientManager>();
    client_id_ = client_manager_->create_client_id();
    send_query(td_api::make_object<td_api::getOption>("version"), {});
    spdlog::get("output")->info("TdInterface initialized");
}

void TdInterface::loop()
{
    spdlog::get("logger")->debug("Starting TdInterface loop...");
    while (true)
    {
        if (need_restart_)
        {
            spdlog::get("logger")->warn("Restarting TdInterface...");
            restart();
        }
        else
        {
            auto response = client_manager_->receive(10);
            process_response(std::move(response));
        }
    }
}

void TdInterface::send_query(object_ptr<td_api::Function> f, std::function<void(Object)> handler)
{
    auto query_id = next_query_id();
    if (handler)
    {
        handlers_[query_id] = std::move(handler);
    }
    client_manager_->send(client_id_, query_id, std::move(f));
}

void TdInterface::send_query_check()
{

    auto get_gift = td_api::make_object<td_api::getReceivedGift>(id);
    auto query_id = 123456789;
    client_manager_->send(client_id_, query_id, std::move(get_gift));
    {
        std::lock_guard lk(times_mutex_);
        times[sent_] = std::chrono::steady_clock::now();
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
}

void TdInterface::send_query_upgrade()
{
    auto upgrade_gift = td_api::make_object<td_api::upgradeGift>(bb, id, true, 25000);
    auto query_id = 987654321;
    client_manager_->send(client_id_, query_id, std::move(upgrade_gift));
    {
        std::lock_guard lk(times_mutex_);
        times[sent_] = std::chrono::steady_clock::now();
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t TdInterface::next_query_id()
{
    return ++current_query_id_;
}

void TdInterface::process_response(td::ClientManager::Response response)
{
    if (!response.object)
    {
        return;
    }
    if (response.request_id == 0)
    {
        process_update(std::move(response.object));
        return;
    }
    if (response.request_id == 123456789)
    {
        {
            std::lock_guard lk(times_mutex_);

            auto it = times.find(received_);
            if (it != times.end())
            {
                {
                    auto now = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
                    spdlog::get("logger")->info("[Check] Time taken: {} ms | sent/recieved: {}/{}", duration.count(), sent_, received_);
                    times.erase(it);
                }
            }
        }

        received_.fetch_add(1, std::memory_order_relaxed);
        auto obj = std::move(response.object);
        // Special case for check query
        if (obj->get_id() == td_api::error::ID)
        {
            auto error = td::move_tl_object_as<td_api::error>(obj);
            spdlog::get("logger")->error("[Error] {}", to_string(error));
            return;
        }
        else if (obj->get_id() == td_api::receivedGift::ID)
        {
            auto gift = td::move_tl_object_as<td_api::receivedGift>(obj);
            if (gift->can_be_upgraded_)
            {

                checking.store(false, std::memory_order_relaxed);
                auto upgrade_gift = td_api::make_object<td_api::upgradeGift>(bb, id, false, 25000);
                send_query(std::move(upgrade_gift), [this](Object obj)
                            {
                                            if (obj->get_id() == td_api::error::ID)
                                            {
                                                auto error = td::move_tl_object_as<td_api::error>(obj);
                                                spdlog::get("logger")->error("[Error] {}", to_string(error));
                                                return;
                                            }
                                            else if (obj->get_id() == td_api::upgradeGiftResult::ID)
                                            {
                                                auto upgrade_result = td::move_tl_object_as<td_api::upgradeGiftResult>(obj);
                                                spdlog::get("logger")->info("[Upgrade Result] {}", to_string(upgrade_result));
                                            } });
            }
            spdlog::get("logger")->info("[Gift] ID: {}, Upgradeble: {}, type_id: {}",
                                        gift->received_gift_id_, gift->can_be_upgraded_, gift->gift_->get_id());

            auto sentgift = td::move_tl_object_as<td_api::sentGiftRegular>(gift->gift_);

            spdlog::get("logger")->info("[Gift] ID: {}, starCount: {}, total: {}, usc: {}",
                                        sentgift->gift_->id_, sentgift->gift_->star_count_, sentgift->gift_->total_count_, sentgift->gift_->upgrade_star_count_);
        }
        else
        {
            spdlog::get("logger")->error("[Error] ID = {}", obj->get_id());
        }
        return;
    }

    if (response.request_id == 987654321) {
        {
            std::lock_guard lk(times_mutex_);

            auto it = times.find(received_);
            if (it != times.end())
            {
                {
                    auto now = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
                    spdlog::get("logger")->info("[Check] Time taken: {} ms | sent/recieved: {}/{}", duration.count(), sent_, received_);
                    times.erase(it);
                }
            }
        }
        received_.fetch_add(1, std::memory_order_relaxed);
        auto obj = std::move(response.object);

        if (obj->get_id() == td_api::error::ID)
        {
            auto error = td::move_tl_object_as<td_api::error>(obj);
            if (error->code_ == 400 && error->message_ == "STARGIFT_UPGRADE_UNAVAILABLE") {
                return;
            }
            if (error->code_ == 400 && error->message_ == "Have not enough Telegram Stars")
            {
                // spdlog::get("logger")->error("[Error] not enough");
                return;
            }
            spdlog::get("logger")->error("[Error] {}", to_string(error));
            return;
        }
        else if (obj->get_id() == td_api::upgradeGiftResult::ID)
        {
            auto upgrade_result = td::move_tl_object_as<td_api::upgradeGiftResult>(obj);
            spdlog::get("logger")->info("[Upgrade Result] {}", to_string(upgrade_result));
            checking.store(false, std::memory_order_relaxed);
        }

        return;
    }
    else
    {

        auto it = handlers_.find(response.request_id);
        if (it != handlers_.end())
        {
            it->second(std::move(response.object));
            handlers_.erase(it);
        }
        return;
    }
}

void TdInterface::process_update(object_ptr<td_api::Object> update)
{
    td_api::downcast_call(*update, overloaded(
                                       [this](td_api::updateAuthorizationState &auth_state)
                                       {
                                           authorization_state_ = std::move(auth_state.authorization_state_);
                                           on_authorization_state_update();
                                       },
                                       [this](td_api::updateNewMessage &update_new_message)
                                       {
                                           auto &msg = update_new_message.message_;
                                           if (!msg || !msg->sender_id_)
                                               return;
                                           if (msg->sender_id_->get_id() != td_api::messageSenderUser::ID)
                                               return;
                                           if (msg->is_outgoing_)
                                               return;
                                           auto sender = td_api::move_object_as<td_api::messageSenderUser>(msg->sender_id_);

                                           if (msg->chat_id_ != sender->user_id_)
                                               return; // не личное сообщение


                                           if (msg->content_ && msg->content_->get_id() == td_api::messageText::ID)
                                           {
                                               auto text_msg = td_api::move_object_as<td_api::messageText>(msg->content_);
                                               if (text_msg->text_->text_ == "check"){
                                                test();
                                               }
                                           }
                                       },
                                       [](auto &) {}));
}

void TdInterface::test() {
    auto me = td_api::make_object<td_api::messageSenderUser>(879292729);

    std::string bb = "";
    std::string offset = "";
    auto get_gifts = td_api::make_object<td_api::getReceivedGifts>(
        bb,
        std::move(me),
        false,
        false,
        true,
        false,
        true,
        false,
        offset,
        100
    );
    send_query(std::move(get_gifts), [this](Object obj)
               {
        if (obj->get_id() == td_api::error::ID)
        {
            auto error = td::move_tl_object_as<td_api::error>(obj);
            spdlog::get("logger")->error("[Error] {}", to_string(error));
            return;
        }
        else if (obj->get_id() == td_api::receivedGifts::ID)
        {
            auto gifts = td::move_tl_object_as<td_api::receivedGifts>(obj);
            spdlog::get("logger")->info("[Gifts] Received {} gifts, {}", gifts->total_count_, gifts->next_offset_);
            for (auto &gift : gifts->gifts_)
            {
                spdlog::get("logger")->info("[Gift] ID: {}, Upgradeble: {}, type_id: {}",
                                            gift->received_gift_id_, gift->can_be_upgraded_, gift->gift_->get_id());

                auto sentgift = td::move_tl_object_as<td_api::sentGiftRegular>(gift->gift_);

                spdlog::get("logger")->info("[Gift] ID: {}, starCount: {}, total: {}",
                                            sentgift->gift_->id_, sentgift->gift_->star_count_, sentgift->gift_->total_count_);
            }
        }
        else
        {
            spdlog::get("logger")->error("[Error] ID = {}", obj->get_id());
        } });
}



void TdInterface::check_for_upgrade()
{
    while (true) {
        send_query_check();
        {
            if (!checking) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
}

void TdInterface::upgrade_loop()
{
    while (true)
    {
        send_query_upgrade();
        {
            if (!checking)
            {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
}

void TdInterface::on_authorization_state_update()
{
    spdlog::get("logger")->info("Authorization state update");
    authentication_query_id_++;
    td_api::downcast_call(*authorization_state_, overloaded(
                                                     [this](td_api::authorizationStateReady &)
                                                     {
                                                         are_authorized_ = true;
                                                         on_authorized();
                                                     },
                                                     [this](td_api::authorizationStateLoggingOut &)
                                                     {
                                                         are_authorized_ = false;
                                                         spdlog::get("output")->info("Logging out");
                                                     },
                                                     [this](td_api::authorizationStateClosing &)
                                                     {
                                                         spdlog::get("output")->info("Closing");
                                                     },
                                                     [this](td_api::authorizationStateClosed &)
                                                     {
                                                         are_authorized_ = false;
                                                         need_restart_ = true;
                                                         spdlog::get("output")->info("Terminated");
                                                     },
                                                     [this](td_api::authorizationStateWaitPhoneNumber &)
                                                     {
                                                         spdlog::get("output")->info("Enter phone number: ");
                                                         std::string phone_number;
                                                         std::cin >> phone_number;
                                                         send_query(
                                                             td_api::make_object<td_api::setAuthenticationPhoneNumber>(phone_number, nullptr),
                                                             create_authentication_query_handler());
                                                     },
                                                     [this](td_api::authorizationStateWaitEmailAddress &)
                                                     {
                                                         spdlog::get("output")->info("Enter email address: ");
                                                         std::string email_address;
                                                         std::cin >> email_address;
                                                         send_query(td_api::make_object<td_api::setAuthenticationEmailAddress>(email_address),
                                                                    create_authentication_query_handler());
                                                     },
                                                     [this](td_api::authorizationStateWaitEmailCode &)
                                                     {
                                                         spdlog::get("output")->info("Enter email authentication code: ");
                                                         std::string code;
                                                         std::cin >> code;
                                                         send_query(td_api::make_object<td_api::checkAuthenticationEmailCode>(
                                                                        td_api::make_object<td_api::emailAddressAuthenticationCode>(code)),
                                                                    create_authentication_query_handler());
                                                     },
                                                     [this](td_api::authorizationStateWaitCode &)
                                                     {
                                                         spdlog::get("output")->info("Enter authentication code: ");
                                                         std::string code;
                                                         std::cin >> code;
                                                         send_query(td_api::make_object<td_api::checkAuthenticationCode>(code),
                                                                    create_authentication_query_handler());
                                                     },
                                                     [this](td_api::authorizationStateWaitRegistration &)
                                                     {
                                                         std::string first_name;
                                                         std::string last_name;
                                                         spdlog::get("output")->info("Enter your first name: ");
                                                         std::cin >> first_name;
                                                         spdlog::get("output")->info("Enter your last name: ");
                                                         std::cin >> last_name;
                                                         send_query(td_api::make_object<td_api::registerUser>(first_name, last_name, false),
                                                                    create_authentication_query_handler());
                                                     },
                                                     [this](td_api::authorizationStateWaitPassword &)
                                                     {
                                                         spdlog::get("output")->info("Enter authentication password: ");
                                                         std::string password;
                                                         std::getline(std::cin, password);
                                                         send_query(td_api::make_object<td_api::checkAuthenticationPassword>(password),
                                                                    create_authentication_query_handler());
                                                     },
                                                     [this](td_api::authorizationStateWaitOtherDeviceConfirmation &state)
                                                     {
                                                         spdlog::get("output")->info("Confirm this login link on another device: {}", state.link_);
                                                     },
                                                     [this](td_api::authorizationStateWaitTdlibParameters &)
                                                     {
                                                         auto request = td_api::make_object<td_api::setTdlibParameters>();
                                                         request->database_directory_ = "ai_agent_td";
                                                         request->use_message_database_ = true;
                                                         request->use_secret_chats_ = true;
                                                         request->api_id_ = api_id_;
                                                         request->api_hash_ = api_hash_;
                                                         request->system_language_code_ = "en";
                                                         request->device_model_ = "Desktop";
                                                         request->application_version_ = "1.0";
                                                         send_query(std::move(request), create_authentication_query_handler());
                                                     },
                                                     [](auto &){}
                                                     ));
}

void TdInterface::check_authentication_error(Object object)
{
    if (object->get_id() == td_api::error::ID)
    {
        auto error = td::move_tl_object_as<td_api::error>(object);
        spdlog::get("logger")->error("Auth error: {}", to_string(error));
        on_authorization_state_update();
    }
}

std::function<void(TdInterface::Object)> TdInterface::create_authentication_query_handler()
{
    return [this, id = authentication_query_id_](Object object)
    {
        if (id == authentication_query_id_)
        {
            check_authentication_error(std::move(object));
        }
    };
}

void TdInterface::restart()
{
    spdlog::get("logger")->warn("Restarting...");
    client_manager_.reset();

    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(1));
    client_manager_ = std::make_unique<td::ClientManager>();
    client_id_ = client_manager_->create_client_id();
    send_query(td_api::make_object<td_api::getOption>("version"), {});
}

void TdInterface::on_authorized()
{
    spdlog::get("output")->info("Authorization is completed");
    if (on_authorized_callback_)
    {
        on_authorized_callback_();
    }
}