#pragma once
#include "tools.hpp"
#include <rapidjson/document.h>
#include <string>
#include <vector>

class Agent {
public:
    // api_base: e.g. "http://localhost:1234"
    // quick_model: LM Studio model identifier for short/simple questions.
    // deep_model:  LM Studio model identifier for coding, tool use, and reasoning-heavy work.
    // Empty model values let the server use whatever is loaded/default.
    Agent(std::string api_base, std::string working_dir,
          std::string quick_model = "", std::string deep_model = "");

    // Run one user turn; prints the final assistant reply.
    void run(const std::string& user_input);

    // Clear message history (start fresh conversation).
    void reset();

    void set_model(const std::string& m)       { quick_model_ = m; deep_model_ = m; }
    void set_quick_model(const std::string& m) { quick_model_ = m; }
    void set_deep_model(const std::string& m)  { deep_model_ = m; }
    void set_models(const std::string& quick, const std::string& deep) {
        quick_model_ = quick;
        deep_model_ = deep;
    }
    void set_api_base(const std::string& url)  { api_base_ = url; }
    void set_context_limit(size_t chars)       { context_limit_ = chars; }

    const std::string& quick_model() const { return quick_model_; }
    const std::string& deep_model()  const { return deep_model_; }

    bool        task_done()    const { return tools_.task_done(); }
    std::string task_summary() const { return tools_.task_summary(); }
    void        reset_task()         { tools_.reset_task(); }

    // Change working directory, reset history, then inject a repo file-tree
    // snapshot so the model immediately knows the codebase layout.
    void set_repo(const std::string& path = "");

private:
    void inject_repo_map();
    void trim_history();

    enum class ModelRoute { Quick, Deep };

    ModelRoute select_model_for(const std::string& user_input) const;
    const std::string& model_for(ModelRoute route) const;
    const char* route_name(ModelRoute route) const;

    // Returns the parsed response Document.
    rapidjson::Document chat(const std::vector<rapidjson::Document>& messages, ModelRoute route);

    std::string http_post(const std::string& path, const rapidjson::Document& body);

    std::string  api_base_;
    std::string  quick_model_;
    std::string  deep_model_;
    Tools        tools_;

    // Each element is a self-contained rapidjson Document representing one message.
    std::vector<rapidjson::Document> messages_;

    size_t context_limit_ = 60'000;

    static constexpr int    MAX_TOOL_ROUNDS   = 50;
    static constexpr size_t MIN_TURNS_TO_KEEP = 2;
};
