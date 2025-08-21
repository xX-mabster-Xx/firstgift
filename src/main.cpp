#include "td_interface.hpp"
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <unordered_set>

int main()
{
    const char *api_id_env = std::getenv("TG_API_ID");
    const char *api_hash_env = std::getenv("TG_API_HASH");

    auto output = spdlog::stdout_color_mt("output");
    output->set_pattern("%v");
    auto logger = spdlog::stderr_color_mt("logger");
    logger->set_level(spdlog::level::debug);

    if (!api_id_env || !api_hash_env)
    {
        logger->error("TG_API_ID or TG_API_HASH not set in environment");
        return 1;
    }

    int32_t api_id = std::stoi(api_id_env);
    std::string api_hash = api_hash_env;


    TdInterface tg(api_id, api_hash);
    tg.set_on_authorized_callback([&tg, output]()
                                  { std::thread([&tg, output]()
                                                {
            output->info("enter commands: ");
            std::string command;
            while (std::getline(std::cin, command)) {
                if (command == "on") {
                    output->info("[State] Set to ACTIVE");
                }
                else if (command == "test")
                { 
                    tg.test();
                }
                else if (command == "upgrade")
                { 
                    std::string identifier;
                    int price;
                    std::cout << "ID: ";
                    std::cin >> identifier;
                    std::cout << "price: ";
                    std::cin >> price;
                    tg.send_query_upgrade(identifier, price);
                }
                else if (command == "upg")
                {
                    if (tg.checking.load(std::memory_order_relaxed))
                    {
                        output->info("[State] Already checking for upgrades");
                        continue;
                    }

                    int millis = 25;
                    std::cout << "time: ";
                    std::cin >> millis;
                    // очистим \n после >>, чтобы не ломать следующий getline
                    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

                    // === 1) Читаем gifts.json ===
                    std::vector<std::pair<std::string, std::int64_t>> gifts;
                    try {
                        std::ifstream in("gifts.json");
                        if (!in.is_open()) {
                            throw std::runtime_error("can't open gifts.json");
                        }
                        nlohmann::json j;
                        in >> j;
                        for (const auto& e : j) {
                            gifts.emplace_back(
                                e.at("id").get<std::string>(),
                                e.at("price").get<std::int64_t>()
                            );
                        }
                    } catch (const std::exception& e) {
                        spdlog::get("logger")->error("[upg] failed to read gifts.json: {}", e.what());
                        continue;
                    }

                    // === 2) Находим канальные received_gift_id и вызываем test(<chat_id>) ===
                    auto parse_chat_id = [](const std::string& s) -> std::optional<long long> {
                        // ожидаем вид "-100XXXXXXXXX_YYYY..."
                        if (s.rfind("-100", 0) == 0) {
                            auto pos = s.find('_');
                            if (pos != std::string::npos) {
                                try {
                                    long long chat_id = std::stoll(s.substr(0, pos));
                                    return chat_id;
                                } catch (...) {}
                            }
                        }
                        return std::nullopt;
                    };

                    std::unordered_set<long long> channels;
                    for (const auto& [id, price] : gifts) {
                        if (auto cid = parse_chat_id(id)) {
                            channels.insert(*cid);
                        }
                    }
                    for (auto cid : channels) {
                        tg.test(static_cast<td_api::int64>(cid));
                    }

                    // === 3) Стартуем апгрейд-цикл с этим набором ===
                    tg.checking.store(true, std::memory_order_release);
                    std::thread([&tg, millis, gifts = std::move(gifts)]() mutable {
                        tg.upgrade_loop(millis, gifts);
                    }).detach();
                }
                else if (command == "stop")
                {

                    output->info("");
                    tg.checking.store(false, std::memory_order_relaxed);
                }
                else {
                    output->info("[Unknown Command] Use 'on' or 'off'");
                }
            } })
                                        .detach(); });
    output->info("Starting loop...");
    tg.loop();

    return 0;
}
