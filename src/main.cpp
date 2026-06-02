#include "agent.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
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
        "  /model <name>    - set both quick and deep models to the same model\n"
        "  /models <quick> <deep>\n"
        "                   - set quick and deep model identifiers\n"
        "  /quickmodel <name>\n"
        "                   - set the quick-question model\n"
        "  /deepmodel <name>\n"
        "                   - set the deeper-reasoning model\n"
        "  /api <url>       - change LLM server address (e.g. /api http://192.168.1.50:1234)\n"
        "  /context <chars> - set context trim limit (default 60000)\n"
        "  startup config   - load cppagent_config.txt, CppAgent/config.txt, or --config <path>\n"
        "  /help            - show this message\n"
        "  exit / quit      - exit\n\n";
}

struct StartupConfig {
    std::string working_dir = fs::current_path().string();
    std::string api_base    = "http://192.168.0.209:1234";
    std::string quick_model = "google/gemma-4-e4b";
    std::string deep_model;
    size_t      context_limit = 60'000;
    int         auto_max = 50;
};

static std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::string lowercase_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static bool parse_size_value(const std::string& text, size_t& out) {
    try {
        size_t pos = 0;
        unsigned long long value = std::stoull(text, &pos);
        if (trim_copy(text.substr(pos)).empty()) {
            out = (size_t)value;
            return true;
        }
    } catch (...) {}
    return false;
}

static bool parse_int_value(const std::string& text, int& out) {
    try {
        size_t pos = 0;
        int value = std::stoi(text, &pos);
        if (trim_copy(text.substr(pos)).empty()) {
            out = value;
            return true;
        }
    } catch (...) {}
    return false;
}

static void apply_config_entry(StartupConfig& config, const std::string& key,
                               const std::string& value, int line_number) {
    std::string k = lowercase_copy(trim_copy(key));
    std::string v = trim_copy(value);

    if (k == "working_dir" || k == "repo" || k == "repo_root") {
        config.working_dir = v;
    } else if (k == "api_base" || k == "api_url" || k == "model_ip" || k == "server") {
        config.api_base = v;
    } else if (k == "model") {
        config.quick_model = v;
        config.deep_model = v;
    } else if (k == "quick_model" || k == "quickmodel") {
        config.quick_model = v;
    } else if (k == "deep_model" || k == "deepmodel" || k == "reasoning_model") {
        config.deep_model = v;
    } else if (k == "context_limit" || k == "context_length" || k == "context") {
        size_t parsed = 0;
        if (parse_size_value(v, parsed)) config.context_limit = parsed;
        else std::cerr << "[config] line " << line_number << ": invalid context value '" << v << "'\n";
    } else if (k == "auto_max" || k == "automax") {
        int parsed = 0;
        if (parse_int_value(v, parsed)) config.auto_max = std::max(1, parsed);
        else std::cerr << "[config] line " << line_number << ": invalid auto_max value '" << v << "'\n";
    } else {
        std::cerr << "[config] line " << line_number << ": unknown key '" << key << "'\n";
    }
}

static bool load_startup_config(const fs::path& path, StartupConfig& config) {
    std::ifstream in(path);
    if (!in) return false;

    std::string line;
    int line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        std::string trimmed = trim_copy(line);
        if (trimmed.empty() || trimmed.starts_with("#")) continue;

        auto eq = trimmed.find('=');
        if (eq == std::string::npos) {
            std::cerr << "[config] line " << line_number << ": expected key=value\n";
            continue;
        }

        apply_config_entry(config, trimmed.substr(0, eq), trimmed.substr(eq + 1), line_number);
    }

    return true;
}

static fs::path default_config_path() {
    fs::path cwd_config = fs::current_path() / "cppagent_config.txt";
    if (fs::exists(cwd_config)) return cwd_config;

    fs::path app_config = fs::current_path() / "CppAgent" / "config.txt";
    if (fs::exists(app_config)) return app_config;

    return {};
}

static bool split_two_args(const std::string& input, std::string& first, std::string& second) {
    auto ns = input.find_first_not_of(" \t");
    if (ns == std::string::npos) return false;
    auto split = input.find_first_of(" \t", ns);
    if (split == std::string::npos) return false;

    first = input.substr(ns, split - ns);

    auto second_start = input.find_first_not_of(" \t", split);
    if (second_start == std::string::npos) return false;
    second = input.substr(second_start);
    return true;
}

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif

    StartupConfig config;
    fs::path config_path;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "[error] usage: --config <path>\n";
                return 1;
            }
            config_path = argv[++i];
        } else {
            positional.push_back(arg);
        }
    }

    if (config_path.empty()) config_path = default_config_path();
    if (!config_path.empty()) {
        if (load_startup_config(config_path, config))
            std::cout << "[config] loaded " << config_path.string() << "\n";
        else
            std::cerr << "[config] could not load " << config_path.string() << "\n";
    }

    if (positional.size() > 0) config.working_dir = positional[0];
    if (positional.size() > 1) config.api_base = positional[1];
    if (positional.size() > 2) config.quick_model = positional[2];
    if (positional.size() > 3) config.deep_model = positional[3];
    if (config.deep_model.empty()) config.deep_model = config.quick_model;

    // Updated initialization print statement for better readability
    std::cout << "========================================\n";
    std::cout << "CppAgent Initializing:\n";
    std::cout << "  Working Directory: " << config.working_dir << "\n";
    std::cout << "  API Base URL:      " << config.api_base << "\n";
    if (!config.quick_model.empty()) {
        std::cout << "  Quick Model:       " << config.quick_model << "\n";
    } else {
        std::cout << "  Quick Model:       (Default/None)\n";
    }
    if (!config.deep_model.empty()) {
        std::cout << "  Deep Model:        " << config.deep_model << "\n";
    } else {
        std::cout << "  Deep Model:        (Default/None)\n";
    }
    std::cout << "  Context Limit:     " << config.context_limit << "\n";
    std::cout << "  Auto Max:          " << config.auto_max << "\n";
    std::cout << "========================================\n";

    Agent agent(config.api_base, config.working_dir, config.quick_model, config.deep_model);
    agent.set_context_limit(config.context_limit);

    int auto_max = config.auto_max;

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
            std::cout << "[quick and deep models set to: " << line.substr(7) << "]\n";
            continue;
        }

        if (line.starts_with("/models ")) {
            std::string quick;
            std::string deep;
            if (!split_two_args(line.substr(8), quick, deep)) {
                std::cerr << "[error] usage: /models <quick_model> <deep_model>\n";
                continue;
            }
            agent.set_models(quick, deep);
            std::cout << "[quick model set to: " << quick << "]\n";
            std::cout << "[deep model set to: " << deep << "]\n";
            continue;
        }

        if (line.starts_with("/quickmodel ")) {
            agent.set_quick_model(line.substr(12));
            std::cout << "[quick model set to: " << line.substr(12) << "]\n";
            continue;
        }

        if (line.starts_with("/deepmodel ")) {
            agent.set_deep_model(line.substr(11));
            std::cout << "[deep model set to: " << line.substr(11) << "]\n";
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
