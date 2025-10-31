#include "td_interface.hpp"

#include <fmt/format.h>
#include <sstream>
#include <iomanip>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <fmt/color.h>
#include <fmt/core.h>
#include <iostream>
#include <sstream>
#include <codecvt>
#include <locale>
#include <filesystem>
#include <chrono>
#include <unordered_set>


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

void TdInterface::send_query_upgrade(const std::string& received_gift_id, int price = 25000)
{
    auto upgrade_gift = td_api::make_object<td_api::upgradeGift>(bb, received_gift_id, false, price);
    
    constexpr std::int64_t BASE = 900000000;
    std::int64_t query_id = BASE;

    bool full_parse_ok = false;
    try {
        std::size_t idx = 0;
        long long v = std::stoll(received_gift_id, &idx, 10);
        if (idx == received_gift_id.size() && v >= 0) {
            query_id = BASE + static_cast<std::int64_t>(v);
            full_parse_ok = true;
        }
    } catch (...) {
    }

    if (!full_parse_ok) {
        const std::string& s = received_gift_id;

        std::size_t j = s.size();
        while (j > 0 && std::isdigit(static_cast<unsigned char>(s[j - 1]))) {
            --j;
        }
        const std::size_t digits_len = s.size() - j;

        if (digits_len > 0) {
            std::string tail = s.substr(j, digits_len);
            if (tail.size() > 6) {
                tail = tail.substr(tail.size() - 6);
            }
            try {
                std::int64_t suffix = std::stoll(tail);
                query_id = BASE + suffix;
            } catch (...) {
            }
        }
    }

    client_manager_->send(client_id_, query_id, std::move(upgrade_gift));
    {
        std::lock_guard lk(times_mutex_);
        times[sent_] = std::chrono::steady_clock::now();
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
}

void TdInterface::send_query_upgrade()
{
    auto upgrade_gift = td_api::make_object<td_api::upgradeGift>(bb, id, false, 25000);
    auto query_id = 987654321;
    client_manager_->send(client_id_, query_id, std::move(upgrade_gift));
    {
        std::lock_guard lk(times_mutex_);
        times[sent_] = std::chrono::steady_clock::now();
    }
    sent_.fetch_add(1, std::memory_order_relaxed);
}

td_api::object_ptr<td_api::MessageSender> TdInterface::make_sender(td_api::int64 id) {
    if (id < 0) {
        return td_api::make_object<td_api::messageSenderChat>(id);
    } else {
        return td_api::make_object<td_api::messageSenderUser>(id);
    }
}


void TdInterface::buy_loop(int millis, td_api::int64 owner_id)
{
    auto seen = std::make_shared<std::unordered_set<td_api::int64>>();
    spdlog::get("output")->info("[buy_loop] Starting to {} (delay={})", owner_id, millis);


    while (buying.load()) {
        send_query(
            td_api::make_object<td_api::getAvailableGifts>(),
            [this, owner_id, seen](Object obj)
            {
                if (obj->get_id() == td_api::error::ID) {
                    auto e = td::move_tl_object_as<td_api::error>(obj);
                    spdlog::get("logger")->warn("[buy_loop] getAvailableGifts error: {}", to_string(e));
                    return;
                }
                if (obj->get_id() != td_api::availableGifts::ID) {
                    spdlog::get("logger")->warn("[buy_loop] unexpected object id={} on getAvailableGifts", obj->get_id());
                    return;
                }

                auto list = td::move_tl_object_as<td_api::availableGifts>(obj);
                std::vector<td_api::int64> to_buy;

                for (auto &ag : list->gifts_) {
                    if (!ag || !ag->gift_) continue;
                    const auto &g = ag->gift_;

                    const bool is_limited = (g->overall_limits_->total_count_ > 0);
                    const bool has_stock  = (g->overall_limits_->remaining_count_ > 0);

                    if (!is_limited || !has_stock) continue;

                    const td_api::int64 &gift_id = g->id_;
                    if (seen->insert(gift_id).second) {
                        to_buy.push_back(gift_id);
                        spdlog::get("output")->info("[buy_loop] NEW limited gift detected: id={} price={} remaining={}",
                                                     gift_id, g->star_count_, g->overall_limits_->remaining_count_);
                    }
                }
                if (to_buy.size() <= 0) {
                    spdlog::get("output")->info("[buy_loop] No availible gifts");
                }
                return;
                for (const auto &gift_id : to_buy) {
                    auto req = td_api::make_object<td_api::sendGift>(
                        gift_id,
                        make_sender(owner_id),   
                        td_api::make_object<td_api::formattedText>(
                            std::string{}, std::vector<td_api::object_ptr<td_api::textEntity>>{}
                        ), 
                        true,
                        false
                    );

                    send_query(std::move(req),
                        [gift_id](Object res)
                        {
                            if (res->get_id() == td_api::error::ID) {
                                auto e = td::move_tl_object_as<td_api::error>(res);
                                spdlog::get("logger")->warn("[buy_loop] sendGift id={} -> error: {}", gift_id, to_string(e));
                            } else {
                                spdlog::get("output")->info("[buy_loop] sendGift id={} -> OK ({}", gift_id, res->get_id());
                            }
                        }
                    );
                }
            }
        );

        std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    }
}

void TdInterface::upgrade_loop(int millis)
{
    using Item = std::pair<std::string, std::int64_t>;
    const std::vector<Item> gifts = {
        {"700279", 25000}, // kirpich-
        {"727170", 25000}, // soska
        {"689019", 25000}, // knopka-
        {"691933", 25000}, // ruki-
        {"700281", 25000}, // doshik
        {"700280", 25000}, // govno
        {"716987", 25000}, // medal
        {"727106", 25000}, // eskimo
        {"727107", 25000}, // plmbir
        {"716986", 25000}, // marka
        {"691894", 25000}, // socks-
        {"698277", 25000} // klever
    };
    std::size_t i = 0;
    while (checking.load()) {
        const auto& [id, price] = gifts[i++ % gifts.size()];
        send_query_upgrade(id, price);
        std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    }
}

void TdInterface::upgrade_loop(int millis, const std::vector<std::pair<std::string, std::int64_t>>& gifts)
{
    if (gifts.empty()) {
        spdlog::get("logger")->warn("[upgrade_loop] gifts list is empty");
        return;
    }
    std::size_t i = 0;
    while (checking.load(std::memory_order_acquire)) {
        const auto& [id, price] = gifts[i++ % gifts.size()];
        send_query_upgrade(id, static_cast<int>(price));
        std::this_thread::sleep_for(std::chrono::milliseconds(millis));
    }
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
                    spdlog::get("logger")->info("[Check] Time taken: {} ms | sent/recieved: {}/{}", duration.count(), sent_.load(), received_.load());
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
                                        sentgift->gift_->id_, sentgift->gift_->star_count_, sentgift->gift_->overall_limits_->total_count_, sentgift->gift_->upgrade_star_count_);
        }
        else
        {
            spdlog::get("logger")->error("[Error] ID = {}", obj->get_id());
        }
        return;
    }

    if (response.request_id > 900000000) {
        std::string to_log;
        {
            std::lock_guard lk(times_mutex_);

            auto it = times.find(received_);
            if (it != times.end())
            {
                {
                    auto now = std::chrono::steady_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - it->second);
                    std::ostringstream oss;
                    oss << "Time taken("
                        << response.request_id - 900000000
                        << "): "
                        << std::setw(3) << std::right << duration.count() // по умолчанию заполняется пробелами
                        << " ms | sent/received: "
                        << sent_
                        << "/"
                        << received_ << " ";
                    to_log += oss.str();
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
                to_log += "E(400): Unavailable";
            }
            else if (error->code_ == 400 && error->message_ == "Have not enough Telegram Stars")
            {
                to_log += "E(400):" + fmt::format(fg(fmt::color::red)," Not enough");
            }
            else {
                to_log += "E(" + std::to_string(error->code_) + "): " + error->message_;
            }
        }
        else if (obj->get_id() == td_api::upgradeGiftResult::ID)
        {
            auto upgrade_result = td::move_tl_object_as<td_api::upgradeGiftResult>(obj);
            to_log += "Success: " + to_string(upgrade_result);
            // checking.store(false, std::memory_order_relaxed);
        }
        else {
            to_log += "Recieved: " + std::to_string(obj->get_id());
        }

        spdlog::get("logger")->info("[Response] {}", to_log);
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
                                           if (msg->is_outgoing_ && msg->chat_id_ != 879292729)
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

                                                if (msg->chat_id_ == 879292729 && text_msg->text_->text_ == "stop") {
                                                    checking.store(false, std::memory_order_relaxed);
                                                }
                                                
                                                else if (msg->chat_id_ == 879292729 && text_msg->text_->text_ == "upg") {
                                                    
                                                    if (checking.load(std::memory_order_relaxed))
                                                    {
                                                        spdlog::get("output")->info("[State] Already checking for upgrades");
                                                    }
                                                    else {
                                                        int millis = 50;
                                                        checking.store(true, std::memory_order_relaxed);
                                                        std::thread([this, millis]()
                                                                    { upgrade_loop(millis); })
                                                            .detach();
                                                    }
                                                }
                                           }
                                       },
                                       [](auto &) {}));
}


using td_api::int32;
using td_api::int64;

void TdInterface::test()
{
    // Получаем текущего пользователя
    send_query(td_api::make_object<td_api::getMe>(),
               [this](Object obj)
               {
                   if (obj->get_id() == td_api::error::ID)
                   {
                       auto e = td::move_tl_object_as<td_api::error>(obj);
                       spdlog::get("logger")->error("[test] getMe error: {}", to_string(e));
                       return;
                   }
                   if (obj->get_id() != td_api::user::ID)
                   {
                       spdlog::get("logger")->error("[test] getMe unexpected object id={}", obj->get_id());
                       return;
                   }

                   auto me = td::move_tl_object_as<td_api::user>(obj);
                   td_api::int64 my_id = me->id_;
                   spdlog::get("logger")->info("[test] using my user_id={}", my_id);

                   // Дальше используем уже существующую перегрузку
                   this->test(my_id);
               });
}

void TdInterface::test(td_api::int64 owner_user_id) {
    namespace fs = std::filesystem;

    const fs::path out_dir = "gifts_channel";
    std::error_code ec;
    fs::create_directories(out_dir, ec); // ок, если уже существует

    // Сохраняем стикер под ИМЕНЕМ ПОЛУЧЕННОГО ПОДАРКА (received_gift_id_, это string)
    auto save_sticker = [this, out_dir](const td_api::object_ptr<td_api::sticker>& st,
                                        const std::string& received_gift_id) {
        if (!st || !st->sticker_) return;

        int32 file_id = st->sticker_->id_; // TDLib file_id
        send_query(td_api::make_object<td_api::downloadFile>(file_id, /*priority*/32, /*offset*/0, /*limit*/0, /*synchronous*/true),
                   [this, out_dir, received_gift_id](Object obj) {
            if (obj->get_id() != td_api::file::ID) {
                spdlog::get("logger")->warn("[StickerSave] not a File (gift_id={})", received_gift_id);
                return;
            }
            auto file = td::move_tl_object_as<td_api::file>(obj);
            if (!file->local_ || file->local_->path_.empty()) {
                spdlog::get("logger")->warn("[StickerSave] no local path (gift_id={})", received_gift_id);
                return;
            }
            try {
                fs::path src_path(file->local_->path_);
                fs::path dst = out_dir / (received_gift_id + src_path.extension().string());
                if (!fs::exists(dst)) {
                    fs::copy_file(src_path, dst, fs::copy_options::none);
                }
            } catch (const std::exception& e) {
                spdlog::get("logger")->warn("[StickerSave] exception: {} (gift_id={})", e.what(), received_gift_id);
            }
        });
    };

    const std::string bb;                  // business_connection_id пустой

    auto fetch_page = std::make_shared<std::function<void(std::string)>>();

    *fetch_page = [this, fetch_page, owner_user_id, bb, save_sticker](std::string offset) {
        td_api::object_ptr<td_api::MessageSender> sender;
        if (owner_user_id < 0) {
            sender = td_api::make_object<td_api::messageSenderChat>(owner_user_id);
        } else {
            sender = td_api::make_object<td_api::messageSenderUser>(owner_user_id);
        }
        auto req = td_api::make_object<td_api::getReceivedGifts>(
            bb,
            std::move(sender),
			/*collection_id*/0,	
            /*exclude_unsaved*/false,
            /*exclude_saved*/false,
            /*exclude_unlimited*/false,
            /*exclude_upgradable*/false,
            /*exclude_non_upgradable*/false,
            /*exclude_upgraded*/false,
            /*sort_by_price*/false,
			false,
			false,
            offset,
            /*limit*/100
        );

        send_query(std::move(req), [this, fetch_page, save_sticker](Object obj) {
            if (obj->get_id() == td_api::error::ID) {
                auto e = td::move_tl_object_as<td_api::error>(obj);
                spdlog::get("logger")->error("[getReceivedGifts] {}", to_string(e));
                return;
            }
            if (obj->get_id() != td_api::receivedGifts::ID) {
                spdlog::get("logger")->error("[getReceivedGifts] unexpected object {}", obj->get_id());
                return;
            }

            auto gifts = td::move_tl_object_as<td_api::receivedGifts>(obj);

            for (auto &rg : gifts->gifts_) {
                if (!rg || !rg->gift_) continue;

                const std::string received_id = rg->received_gift_id_; // <-- ВАЖНО: это string

                switch (rg->gift_->get_id()) {
                    case td_api::sentGiftRegular::ID: {
                        auto reg = td::move_tl_object_as<td_api::sentGiftRegular>(rg->gift_);
                        if (!reg || !reg->gift_) break;

                        // ЛОГ: только id полученного подарка
                        spdlog::get("output")->info("gift_id={} upg_price={}", received_id, reg->gift_->upgrade_star_count_);

                        // Сохранить соответствующий стикер под именем id подарка
                        save_sticker(reg->gift_->sticker_, received_id);
                        break;
                    }
                    case td_api::sentGiftUpgraded::ID: {
                        auto up = td::move_tl_object_as<td_api::sentGiftUpgraded>(rg->gift_);
                        if (!up || !up->gift_) break;
                        const auto &g = up->gift_;

                        const char* model    = (g->model_    && !g->model_->name_.empty())    ? g->model_->name_.c_str()    : "-";
                        const char* backdrop = (g->backdrop_ && !g->backdrop_->name_.empty()) ? g->backdrop_->name_.c_str() : "-";
                        const char* symbol   = (g->symbol_   && !g->symbol_->name_.empty())   ? g->symbol_->name_.c_str()   : "-";

                        // ЛОГ: id полученного подарка + важные поля уникального
                        spdlog::get("output")->info("gift_id={} model={} backdrop={} symbol={}",
                                                    received_id, model, backdrop, symbol);
                        break;
                    }
                    default:
                        spdlog::get("output")->info("gift_id={} type={}", received_id, rg->gift_->get_id());
                        break;
                }
            }

            if (!gifts->next_offset_.empty()) {
                (*fetch_page)(gifts->next_offset_);
            }
        });
    };

    (*fetch_page)("");
}


void TdInterface::check_for_upgrade()
{
    while (true) {
        send_query_check();
        {
            if (!checking.load()) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
