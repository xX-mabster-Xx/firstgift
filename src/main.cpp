#include "td_interface.hpp"
#include <iostream>
#include <memory>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <nlohmann/json.hpp>
#include <thread>

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
                else if (command == "check")
                { 
                    tg.checking = true;
                    tg.check_for_upgrade();
                }
                else if (command == "stop")
                {

                    output->info("");
                    std::lock_guard<std::mutex> lock(tg.checkin_mutex_);
                    tg.checking = false;
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
