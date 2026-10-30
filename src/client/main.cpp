#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app.h"
#include "p2p_app.h"

namespace {

void usage(const char* prog) {
    std::printf(
        "usage:\n"
        "  CS mode:  %s --user NAME [--host ADDR] [--port N] [--room NAME]\n"
        "  P2P mode: %s --p2p --user NAME [--sig HOST:PORT] [--listen N] [--data DIR]\n"
        "\n"
        "common options:\n"
        "  --user NAME         login name (2-32 chars, [A-Za-z0-9_-])\n"
        "  --password PASS     password (prompted when omitted)\n"
        "  --no-color          disable colors\n"
        "  -h, --help          show this help\n"
        "\n"
        "cs mode:\n"
        "  --host ADDR         server address (default 127.0.0.1)\n"
        "  --port N            server port (default 7700)\n"
        "  --room NAME         room to join (default public)\n"
        "  --plain             line mode (no raw terminal ui)\n"
        "\n"
        "p2p mode:\n"
        "  --sig HOST:PORT     signaling server (default 127.0.0.1:7900)\n"
        "  --listen N          tcp port to accept peers (default 7800)\n"
        "  --data DIR          local data dir (default ./p2pdata)\n"
        "  --force-relay       skip direct connect, always use relay (debug)\n",
        prog, prog);
}

}  // namespace

int main(int argc, char** argv) {
    chat::ClientOptions opt;
    chat::P2POptions p2p;
    bool p2p_mode = false;
    bool have_password = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (a == "--plain") {
            opt.plain = true;
            continue;
        }
        if (a == "--no-color") {
            opt.no_color = true;
            p2p.no_color = true;
            continue;
        }
        if (a == "--p2p") {
            p2p_mode = true;
            continue;
        }
        if (a == "--force-relay") {
            p2p.force_relay = true;
            continue;
        }
        if (i + 1 >= argc) {
            std::fprintf(stderr, "chatt: %s requires a value\n", a.c_str());
            return 2;
        }
        const std::string v = argv[++i];
        if (a == "--host") opt.host = v;
        else if (a == "--user") {
            opt.user = v;
            p2p.user = v;
        } else if (a == "--password") {
            opt.password = v;
            p2p.password = v;
            have_password = true;
        } else if (a == "--room") opt.room = v;
        else if (a == "--port") opt.port = static_cast<uint16_t>(std::atoi(v.c_str()));
        else if (a == "--sig") {
            const size_t colon = v.rfind(':');
            if (colon == std::string::npos) {
                std::fprintf(stderr, "chatt: --sig wants HOST:PORT\n");
                return 2;
            }
            p2p.sig_host = v.substr(0, colon);
            p2p.sig_port = static_cast<uint16_t>(std::atoi(v.c_str() + colon + 1));
        } else if (a == "--listen") {
            p2p.listen_port = static_cast<uint16_t>(std::atoi(v.c_str()));
        } else if (a == "--data") {
            p2p.data_dir = v;
        } else {
            std::fprintf(stderr, "chatt: unknown option %s\n", a.c_str());
            return 2;
        }
    }

    if (p2p_mode) {
        if (!have_password) {
            std::fprintf(stderr, "password for %s: ", p2p.user.c_str());
            std::fflush(stderr);
            chat::read_secret_line(p2p.password);
        }
        chat::P2PApp app(p2p);
        return app.run();
    }

    chat::App app(opt);
    return app.run();
}
