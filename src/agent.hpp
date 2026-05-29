#pragma once
#include "tools.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class Agent {
public:
    // api_base: e.g. "http://localhost:1234"
    // model:    LM Studio model identifier (empty = use whatever is loaded)
    Agent(std::string api_base, std::string working_dir, std::string model = "");

    // Run one user turn; prints the final assistant reply.
    void run(const std::string& user_input);

    // Clear message history (start fresh conversation).
    void reset();

    void set_model(const std::string& m)       { model_ = m; }
    void set_api_base(const std::string& url)  { api_base_ = url; }
    void set_context_limit(size_t chars)       { context_limit_ = chars; }

    bool        task_done()    const { return tools_.task_done(); }
    std::string task_summary() const { return tools_.task_summary(); }
    void        reset_task()         { tools_.reset_task(); }

    // Change working directory, reset history, then inject a repo file-tree
    // snapshot so the model immediately knows the codebase layout.
    // Call with no argument to re-scan (and re-inject) the current directory.
    void set_repo(const std::string& path = "");

private:
    // Append a user+assistant exchange that gives the model the repo file tree.
    void inject_repo_map();

    void trim_history();
    nlohmann::json chat(const std::vector<nlohmann::json>& messages);
    std::string http_post(const std::string& path, const nlohmann::json& body);

    std::string  api_base_;
    std::string  model_;
    Tools        tools_;
    std::vector<nlohmann::json> messages_;
    size_t       context_limit_ = 60'000;

    static constexpr int    MAX_TOOL_ROUNDS   = 50;
    static constexpr size_t MIN_TURNS_TO_KEEP = 2;
};
