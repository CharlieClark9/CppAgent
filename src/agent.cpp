#include "agent.hpp"
#define CPPHTTPLIB_NO_SSL
#include <httplib.h>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <stdexcept>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// System prompt
// ---------------------------------------------------------------------------

static const char* SYSTEM_PROMPT = R"(You are CppAgent, an expert coding assistant with direct access to the file system.

Tools available:
- list_files       — list files in a directory (use this FIRST to understand repo layout)
- read_file        — read a whole file
- read_file_range  — read a specific line range (prefer this for large files)
- write_file       — create or overwrite a file entirely (use for NEW files only)
- edit_file        — replace an exact, unique piece of text (PREFERRED for editing existing files)
- replace_block    — replace a multi-line block between two text anchors (for large sections)
- run_command      — execute shell commands
- search_files     — regex search grouped by file (use to find symbols before reading)
- finish_task      — signal that the task is fully complete, with a summary

Workflow for unfamiliar codebases:
1. list_files to see structure.
2. search_files to locate the relevant code.
3. read_file_range on the exact section you need (not the whole file).
4. Edit existing files with edit_file (or replace_block for big sections); use write_file only for new files.
5. Verify with run_command.
6. When all work is done and verified, call finish_task with a summary.

How to edit existing files (IMPORTANT — read carefully):
- Use edit_file with old_text copied EXACTLY from a recent read_file_range, including indentation.
  old_text must be UNIQUE in the file. If it is not, the edit is rejected — add a few surrounding
  lines until it matches exactly one place, then retry. Nothing is changed on a rejected edit, so
  it is always safe to try again.
- Do NOT count line numbers — edit_file and replace_block use text, not line numbers, so they are
  immune to line drift. You can make several edits in one response safely (each must match unique text).
- To remove or replace a whole multi-line section (e.g. an entire <header>...</header>), use
  replace_block with a distinctive start_anchor and end_anchor (e.g. "<header" and "</header>").
  Do not pick generic anchors like "</div>" that appear many times.
- After editing, the tool reports exactly what was removed. Check it matches your intent; if not, fix it.

Rules:
- Never read an entire large file when search_files + read_file_range will do.
- Prefer edit_file/replace_block over write_file when modifying existing files.
- Always call finish_task when running in autonomous (/auto) mode.
)";

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Agent::Agent(std::string api_base, std::string working_dir, std::string model)
    : api_base_(std::move(api_base))
    , model_(std::move(model))
    , tools_(std::move(working_dir))
{
    messages_.push_back({{"role", "system"}, {"content", SYSTEM_PROMPT}});
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Agent::reset() {
    messages_.clear();
    messages_.push_back({{"role", "system"}, {"content", SYSTEM_PROMPT}});
}

void Agent::set_repo(const std::string& path) {
    namespace fs = std::filesystem;

    if (!path.empty()) {
        // Trim leading/trailing whitespace so copy-pasted paths with spaces work
        std::string trimmed = path;
        auto not_space = [](unsigned char c){ return !std::isspace(c); };
        trimmed.erase(trimmed.begin(), std::find_if(trimmed.begin(), trimmed.end(), not_space));
        trimmed.erase(std::find_if(trimmed.rbegin(), trimmed.rend(), not_space).base(), trimmed.end());

        if (!fs::exists(trimmed)) {
            std::cerr << "[error] path does not exist: " << trimmed << "\n";
            return;
        }
        if (!fs::is_directory(trimmed)) {
            std::cerr << "[error] not a directory: " << trimmed << "\n";
            return;
        }

        try {
            tools_.set_working_dir(fs::canonical(trimmed).string());
        } catch (const fs::filesystem_error& e) {
            std::cerr << "[error] cannot resolve path: " << e.what() << "\n";
            return;
        }
    }

    reset();
    inject_repo_map();
    std::cout << "[repo] root set to: " << tools_.get_working_dir() << "\n";
}

void Agent::inject_repo_map() {
    ToolResult r = tools_.dispatch("list_files", nlohmann::json::object());

    std::string content =
        "Repository root: " + tools_.get_working_dir() + "\n\n"
        "File tree:\n" + r.output;

    // Inject as a user message + brief assistant acknowledgement so the model
    // has the layout in context without needing to call list_files first.
    messages_.push_back({{"role", "user"},      {"content", content}});
    messages_.push_back({{"role", "assistant"}, {"content",
        "Got it. I have the full repository layout and will use it to navigate the codebase."}});
}

void Agent::run(const std::string& user_input) {
    messages_.push_back({{"role", "user"}, {"content", user_input}});

    for (int round = 0; round < MAX_TOOL_ROUNDS; ++round) {
        json response;
        try {
            response = chat(messages_);
        } catch (const std::exception& e) {
            std::cerr << "[error] LLM call failed: " << e.what() << "\n";
            return;
        }

        auto& choice  = response["choices"][0];
        auto& message = choice["message"];
        std::string finish = choice.value("finish_reason", "stop");

        messages_.push_back(message);

        // ── No more tool calls: print reply and return ──────────────────────
        if (finish != "tool_calls" || !message.contains("tool_calls")) {
            std::string content = message.value("content", "");
            if (!content.empty())
                std::cout << "\n" << content << "\n\n";
            trim_history();
            return;
        }

        // ── Execute tool calls ───────────────────────────────────────────────
        for (auto& tc : message["tool_calls"]) {
            std::string id       = tc["id"];
            std::string name     = tc["function"]["name"];
            std::string args_str = tc["function"]["arguments"];

            json args;
            try { args = json::parse(args_str); }
            catch (...) { args = json::object(); }

            std::cout << "[tool] " << name << "(" << args.dump() << ")\n";

            ToolResult result = tools_.dispatch(name, args);

            if (!result.success)
                std::cerr << "  [!] " << result.output << "\n";

            messages_.push_back({
                {"role",         "tool"},
                {"tool_call_id", id},
                {"content",      result.output}
            });
        }
    }

    std::cerr << "[warn] reached max tool rounds (" << MAX_TOOL_ROUNDS << ")\n";
    trim_history();
}

// ---------------------------------------------------------------------------
// History compaction
//
// Strategy: messages_ is structured as:
//   [0]      system prompt          (never removed)
//   [1..N]   turn 1: user + assistant exchanges + tool results
//   [N+1..M] turn 2: user + ...
//   ...
//
// We identify "turns" by user-role messages. When over budget we drop the
// oldest complete turn, keeping at least MIN_TURNS_TO_KEEP recent turns.
// ---------------------------------------------------------------------------

void Agent::trim_history() {
    // Estimate total chars in history
    size_t total = 0;
    for (auto& m : messages_)
        total += m.dump().size();

    if (total <= context_limit_) return;

    // Find indices of all user messages (turn starts), skipping system at [0]
    auto find_user_indices = [&]() {
        std::vector<size_t> idx;
        for (size_t i = 1; i < messages_.size(); ++i)
            if (messages_[i]["role"] == "user")
                idx.push_back(i);
        return idx;
    };

    int trims = 0;
    while (total > context_limit_) {
        auto user_idx = find_user_indices();
        if (user_idx.size() <= MIN_TURNS_TO_KEEP) break;

        // Range to remove: from oldest user message up to (not including) the next user message
        size_t cut_start = user_idx[0];
        size_t cut_end   = user_idx[1];

        size_t removed = 0;
        for (size_t i = cut_start; i < cut_end; ++i)
            removed += messages_[i].dump().size();

        messages_.erase(messages_.begin() + cut_start,
                        messages_.begin() + cut_end);
        total -= removed;
        ++trims;
    }

    if (trims > 0) {
        auto remaining = find_user_indices().size();
        std::cerr << "[context] trimmed " << trims << " old turn(s), "
                  << remaining << " turn(s) remaining in history\n";
    }
}

// ---------------------------------------------------------------------------
// LLM call
// ---------------------------------------------------------------------------

json Agent::chat(const std::vector<json>& messages) {
    json body = {
        {"messages",    json(messages)},
        {"tools",       tools_.definitions()},
        {"tool_choice", "auto"},
        {"temperature", 0.2},
        {"max_tokens",  4096}
    };
    if (!model_.empty()) body["model"] = model_;

    std::string resp_str = http_post("/v1/chat/completions", body);

    json resp = json::parse(resp_str);
    if (resp.contains("error"))
        throw std::runtime_error(resp["error"].value("message", "unknown error"));

    return resp;
}

// ---------------------------------------------------------------------------
// HTTP helper
// ---------------------------------------------------------------------------

std::string Agent::http_post(const std::string& path, const json& body) {
    std::string base = api_base_;
    if (base.starts_with("http://"))  base = base.substr(7);
    if (base.starts_with("https://")) base = base.substr(8);

    std::string host;
    int port = 80;
    if (auto colon = base.rfind(':'); colon != std::string::npos) {
        host = base.substr(0, colon);
        port = std::stoi(base.substr(colon + 1));
    } else {
        host = base;
    }

    httplib::Client cli(host, port);
    cli.set_read_timeout(120);
    cli.set_write_timeout(30);

    auto res = cli.Post(path, body.dump(), "application/json");
    if (!res)
        throw std::runtime_error("HTTP error: " + httplib::to_string(res.error()));
    if (res->status != 200)
        throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " + res->body);

    return res->body;
}
