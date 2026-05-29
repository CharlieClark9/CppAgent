#include "agent.hpp"
#include <iostream>
#include <string>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace fs = std::filesystem;

static void print_help() {
    std::cout <<
        "Commands:\n"
        "  /repo            - scan current repo and inject file tree into context\n"
        "  /repo <path>     - switch to a new repo root, reset history, inject file tree\n"
        "  /reset           - clear conversation history\n"
        "  /auto <task>     - run task autonomously until the model calls finish_task\n"
        "  /automax <n>     - set max /auto iterations (default 50)\n"
        "  /model <name>    - switch model\n"
        "  /api <url>       - change LLM server address (e.g. /api http://192.168.1.50:1234)\n"
        "  /context <chars> - set context trim limit (default 60000)\n"
        "  /help            - show this message\n"
        "  exit / quit      - exit\n\n";
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    std::string working_dir = argc > 1 ? argv[1] : fs::current_path().string();
    std::string api_base    = argc > 2 ? argv[2] : "http://192.168.0.209:1234";
    std::string model       = argc > 3 ? argv[3] : "google/gemma-4-e4b";

    // Updated initialization print statement for better readability
    std::cout << "========================================\n";
    std::cout << "CppAgent Initializing:\n";
    std::cout << "  Working Directory: " << working_dir << "\n";
    std::cout << "  API Base URL:      " << api_base << "\n";
    if (!model.empty()) {
        std::cout << "  Model Identifier:  " << model << "\n";
    } else {
        std::cout << "  Model Identifier:  (Default/None)\n";
    }
    std::cout << "========================================\n";

    Agent agent(api_base, working_dir, model);

    int auto_max = 50;

    std::string line;
    while (true) {
        std::cout << "> ";
        std::cout.flush();

        if (!std::getline(std::cin, line)) break;

        // Strip trailing CR (\r\n on Windows)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "exit" || line == "quit") break;
        if (line.empty()) continue;

        if (line == "/help")  { print_help(); continue; }
        if (line == "/reset") { agent.reset(); std::cout << "[history cleared]\n"; continue; }

        if (line == "/repo") {
            agent.set_repo();
            continue;
        }
        if (line.starts_with("/repo ")) {
            agent.set_repo(line.substr(6));
            continue;
        }

        if (line.starts_with("/model ")) {
            agent.set_model(line.substr(7));
            std::cout << "[model set to: " << line.substr(7) << "]\n";
            continue;
        }

        if (line.starts_with("/api ")) {
            agent.set_api_base(line.substr(5));
            std::cout << "[api base set to: " << line.substr(5) << "]\n";
            continue;
        }

        if (line.starts_with("/context ")) {
            try {
                size_t limit = std::stoull(line.substr(9));
                agent.set_context_limit(limit);
                std::cout << "[context limit set to " << limit << " chars]\n";
            } catch (...) {
                std::cerr << "[error] usage: /context <number of chars>\n";
            }
            continue;
        }

        if (line.starts_with("/automax ")) {
            try {
                auto_max = std::stoi(line.substr(9));
                if (auto_max < 1) auto_max = 1;
                std::cout << "[auto max iterations set to " << auto_max << "]\n";
            } catch (...) {
                std::cerr << "[error] usage: /automax <number>\n";
            }
            continue;
        }

        if (line.starts_with("/auto ")) {
            std::string task = line.substr(6);
            // Trim leading whitespace
            auto ns = task.find_first_not_of(" \t");
            if (ns == std::string::npos) {
                std::cerr << "[error] usage: /auto <task description>\n";
                continue;
            }
            task = task.substr(ns);

            agent.reset_task();
            std::cout << "[auto] starting: " << task << "\n\n";
            agent.run(task);

            int iter = 1;
            while (!agent.task_done() && iter < auto_max) {
                ++iter;
                std::cout << "\n[auto] iteration " << iter << "/" << auto_max << "\n";
                agent.run("Continue with the task. Review what has been done so far and take the next step. "
                          "When everything is fully complete and verified, call finish_task with a summary.");
            }

            if (agent.task_done()) {
                std::cout << "\n[auto] done after " << iter << " iteration(s)\n";
                std::cout << "[auto] summary: " << agent.task_summary() << "\n\n";
            } else {
                std::cerr << "\n[auto] reached max iterations (" << auto_max
                          << ") — task may be incomplete. Type a message to continue manually.\n\n";
            }
            continue;
        }

        agent.run(line);
    }

    return 0;
}