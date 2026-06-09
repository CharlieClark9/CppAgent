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

static void print_help() 
{
    std::cout <<
        "Commands:\n"
        "  /repo            - scan current repo and inject file tree into context\n"
        "  /repo <path>     - switch to a new repo root, reset history, inject file tree\n"
        "  /reset           - clear conversation history\n"
        "  /auto <task>     - run task autonomously until the model calls finish_task\n"
        "  /automax <n>     - set max /auto iterations (default 50)\n"
        "  /quick <name>    - set the quick-question model\n"
        "  /deep  <name>    - set the deeper-reasoning model\n"
        "  /ip <url>        - change LLM server address\n"
        "  /context <chars> - set context trim limit (default 60000)\n"
        "  startup config   - load cppagent_config.txt, CppAgent/config.txt, or --config <path>\n"
        "  /help            - show this message\n"
        "  /exit            - exit\n\n";
}

int main(int argc, char* argv[])
{
    std::string ip = "127.0.0.1:1234";
    std::string repo = "";
    std::string quickModel = "google/gemma-4-e2b";
    std::string deepModel = "google/gemma-4-e2b";

    int contextLimit = 60000;
    int auto_max = 50;

    Agent agent(ip, repo, quickModel, deepModell);
    agent.set_context_limit(contextLimit);


    std::string line;
    while (true) {
        std::cout << "> ";
        std::cout.flush();

        if (!std::getline(std::cin, line)) break;

        // Strip trailing CR (\r\n on Windows)
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (line == "exit")
        {
            break;
        }
        else if (line == "/help")
        {
            print_help();
        }
        else if (line == "/reset")
        {
            agent.reset(); std::cout << "[history cleared]\n";
        }
        else if (line == "/repo")
        {
            agent.set_repo();
        }
        else if (line.starts_with("/repo "))
        {
            agent.set_repo(line.substr(6));
        }
        else if (line.starts_with("/quick "))
        {
            agent.set_quick_model(line.substr(12));
            std::cout << "[quick model set to: " << line.substr(12) << "]\n";
        }
        else if (line.starts_with("/deep "))
        {
            agent.set_deep_model(line.substr(11));
            std::cout << "[deep model set to: " << line.substr(11) << "]\n";
        }
        else if (line.starts_with("/api "))
        {
            agent.set_api_base(line.substr(5));
            std::cout << "[api base set to: " << line.substr(5) << "]\n";
        }
        else if (line.starts_with("/context "))
        {
            try
            {
                size_t limit = std::stoull(line.substr(9));
                agent.set_context_limit(limit);
                std::cout << "[context limit set to " << limit << " chars]\n";
            }
            catch (...)
            {
                std::cerr << "[error] usage: /context <number of chars>\n";
            }
        }

        else if (line.starts_with("/automax "))
        {
            try
            {
                auto_max = std::stoi(line.substr(9));
                if (auto_max < 1) auto_max = 1;
                std::cout << "[auto max iterations set to " << auto_max << "]\n";
            }
            catch (...)
            {
                std::cerr << "[error] usage: /automax <number>\n";
            }
        }

        else if (line.starts_with("/auto "))
        {
            std::string task = line.substr(6);
            // Trim leading whitespace
            auto ns = task.find_first_not_of(" \t");
            if (ns == std::string::npos)
            {
                std::cerr << "[error] usage: /auto <task description>\n";
            }
            task = task.substr(ns);

            agent.reset_task();
            std::cout << "[auto] starting: " << task << "\n\n";
            agent.run(task);

            int iter = 1;
            while (!agent.task_done() && iter < auto_max)
            {
                ++iter;
                std::cout << "\n[auto] iteration " << iter << "/" << auto_max << "\n";
                agent.run("Continue with the task. Review what has been done so far and take the next step. When everything is fully complete and verified, call finish_task with a summary.");
            }

            if (agent.task_done())
            {
                std::cout << "\n[auto] done after " << iter << " iteration(s)\n";
                std::cout << "[auto] summary: " << agent.task_summary() << "\n\n";
            }
            else
            {
                std::cerr << "\n[auto] reached max iterations (" << auto_max << ") — task may be incomplete. Type a message to continue manually.\n\n";
            }
        }

        agent.run(line);
    }

    return 0;
}
