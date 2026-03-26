#include <csignal>
#include <iostream>
#include <thread>

#include "args.hxx"
#include "main/database.h"
#include "main/service/lbug_server.h"
#include "main/service/service_config.h"

using namespace lbug::main;

static LbugServer* gServer = nullptr;

void signalHandler(int /*sig*/) {
    if (gServer) {
        gServer->stop();
    }
}

int main(int argc, char* argv[]) {
    args::ArgumentParser parser("Lbug HTTP service mode server.");
    args::HelpFlag help(parser, "help", "Display this help menu.", {'h', "help"});
    args::Positional<std::string> dbPathFlag(parser, "databasePath",
        "Path to the database directory. Use ':memory:' for in-memory mode.");
    args::ValueFlag<std::string> hostFlag(parser, "host", "Interface to listen on.",
        {"host"}, "127.0.0.1");
    args::ValueFlag<uint32_t> portFlag(parser, "port", "Port to listen on.", {"port"}, 8000u);
    args::ValueFlag<uint32_t> poolSizeFlag(parser, "pool_size",
        "Number of pre-allocated connections.", {"pool_size"}, 4u);
    args::ValueFlag<uint64_t> bpSizeFlag(parser, "default_bp_size",
        "Buffer pool size in MB.", {'d', "default_bp_size"}, static_cast<uint64_t>(-1));
    args::Flag readOnlyFlag(parser, "read_only", "Open database in read-only mode.",
        {'r', "read_only"});

    try {
        parser.ParseCLI(argc, argv);
    } catch (const args::Help&) {
        std::cout << parser;
        return 0;
    } catch (const args::ParseError& e) {
        std::cerr << e.what() << "\n" << parser;
        return 1;
    }

    auto dbPath = args::get(dbPathFlag);
    if (dbPath.empty()) {
        dbPath = ":memory:";
    }

    SystemConfig sysConfig;
    auto bpSize = args::get(bpSizeFlag);
    if (bpSize != static_cast<uint64_t>(-1)) {
        sysConfig.bufferPoolSize = bpSize << 20;
    }
    if (readOnlyFlag) {
        sysConfig.readOnly = true;
    }

    ServiceConfig svcConfig;
    svcConfig.host = args::get(hostFlag);
    svcConfig.port = args::get(portFlag);
    svcConfig.poolSize = args::get(poolSizeFlag);

    try {
        auto db = std::make_unique<Database>(dbPath, sysConfig);
        LbugServer server(std::move(db));
        gServer = &server;

        std::signal(SIGINT, signalHandler);
        std::signal(SIGTERM, signalHandler);

        auto url = server.start(svcConfig);
        std::cout << "Lbug service listening at " << url << std::endl;

        while (server.isRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        gServer = nullptr;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
