#include "agent.hpp"
#define CPPHTTPLIB_NO_SSL
#include <httplib.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <rapidjson/error/en.h>

using namespace rapidjson;

// ---------------------------------------------------------------------------
// Serialisation helpers
// ---------------------------------------------------------------------------

// Serialize any rapidjson Value (or Document) to a JSON string.
static std::string to_string(const Value& v) {
    StringBuffer sb;
    Writer<StringBuffer> w(sb);
    v.Accept(w);
    return sb.GetString();
}

// Deep-copy a Value (or Document) into a new Document.
static Document clone(const Value& src) {
    Document d;
    d.CopyFrom(src, d.GetAllocator());
    return d;
}

// Build a Document from a JSON string; throws on parse error.
static Document parse(const std::string& s) {
    Document d;
    d.Parse(s.c_str(), s.size());
    if (d.HasParseError())
        throw std::runtime_error(std::string("JSON parse error: ") + GetParseError_En(d.GetParseError()));
    return d;
}

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
// Build a message Document  {"role": r, "content": c}
// ---------------------------------------------------------------------------

static Document make_msg(const char* role, const std::string& content) {
    Document d;
    d.SetObject();
    auto& a = d.GetAllocator();
    d.AddMember("role",    Value(role, a),             a);
    d.AddMember("content", Value(content.c_str(), a),  a);
    return d;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Agent::Agent(std::string api_base, std::string working_dir,
             std::string quick_model, std::string deep_model)
    : api_base_(std::move(api_base))
    , quick_model_(std::move(quick_model))
    , deep_model_(std::move(deep_model))
    , tools_(std::move(working_dir))
{
    if (deep_model_.empty()) deep_model_ = quick_model_;
    messages_.push_back(make_msg("system", SYSTEM_PROMPT));
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void Agent::reset() {
    messages_.clear();
    messages_.push_back(make_msg("system", SYSTEM_PROMPT));
}

void Agent::set_repo(const std::string& path) {
    namespace fs = std::filesystem;

    if (!path.empty()) {
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
    Document empty;
    empty.SetObject();
    ToolResult r = tools_.dispatch("list_files", empty);

    std::string content =
        "Repository root: " + tools_.get_working_dir() + "\n\n"
        "File tree:\n" + r.output;

    messages_.push_back(make_msg("user", content));
    messages_.push_back(make_msg("assistant",
        "Got it. I have the full repository layout and will use it to navigate the codebase."));
}

// ---------------------------------------------------------------------------
// Estimate serialized size of a message Document
// ---------------------------------------------------------------------------

static size_t msg_size(const Document& d) {
    return to_string(d).size();
}

void Agent::run(const std::string& user_input) {
    messages_.push_back(make_msg("user", user_input));
    ModelRoute route = select_model_for(user_input);

    const auto& selected_model = model_for(route);
    std::cout << "[model] " << route_name(route) << ": "
              << (selected_model.empty() ? "(server default)" : selected_model) << "\n";

    for (int round = 0; round < MAX_TOOL_ROUNDS; ++round) {
        Document response;
        try {
            response = chat(messages_, route);
        } catch (const std::exception& e) {
            std::cerr << "[error] LLM call failed: " << e.what() << "\n";
            return;
        }

        // choices[0].message
        const Value& choice  = response["choices"][0];
        const Value& message = choice["message"];
        std::string finish   = choice.HasMember("finish_reason") && choice["finish_reason"].IsString()
                               ? choice["finish_reason"].GetString() : "stop";

        // Push a copy of the message into history
        messages_.push_back(clone(message));

        // ── No more tool calls ───────────────────────────────────────────────
        if (finish != "tool_calls" || !message.HasMember("tool_calls")) {
            std::string content;
            if (message.HasMember("content") && message["content"].IsString())
                content = message["content"].GetString();
            if (!content.empty())
                std::cout << "\n" << content << "\n\n";
            trim_history();
            return;
        }

        // ── Execute tool calls ───────────────────────────────────────────────
        for (auto& tc : message["tool_calls"].GetArray()) {
            std::string id       = tc["id"].GetString();
            std::string name     = tc["function"]["name"].GetString();
            std::string args_str = tc["function"]["arguments"].GetString();

            Document args;
            try { args = parse(args_str); }
            catch (...) { args.SetObject(); }

            std::cout << "[tool] " << name << "(" << args_str << ")\n";

            ToolResult result = tools_.dispatch(name, args);

            if (!result.success)
                std::cerr << "  [!] " << result.output << "\n";

            // Build tool-result message
            Document tool_msg;
            tool_msg.SetObject();
            auto& a = tool_msg.GetAllocator();
            tool_msg.AddMember("role",         Value("tool", a),               a);
            tool_msg.AddMember("tool_call_id", Value(id.c_str(), a),           a);
            tool_msg.AddMember("content",      Value(result.output.c_str(), a), a);
            messages_.push_back(std::move(tool_msg));
        }
    }

    std::cerr << "[warn] reached max tool rounds (" << MAX_TOOL_ROUNDS << ")\n";
    trim_history();
}

// ---------------------------------------------------------------------------
// Model routing
// ---------------------------------------------------------------------------

static std::string lowercase_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

static bool contains_any(std::string_view text, const std::vector<std::string_view>& needles) {
    for (auto needle : needles) {
        if (text.find(needle) != std::string_view::npos) return true;
    }
    return false;
}

Agent::ModelRoute Agent::select_model_for(const std::string& user_input) const {
    std::string text = lowercase_copy(user_input);

    const bool likely_continuation = text.starts_with("continue with the task");
    const bool long_prompt = user_input.size() > 350;
    const bool asks_for_steps = contains_any(text, {
        "step by step", "plan", "architecture", "design", "refactor", "implement",
        "change the code", "modify", "edit", "fix", "debug", "bug", "test",
        "build", "compile", "error", "failing", "failure", "review", "analyse",
        "analyze", "reason", "deep", "complex", "multi-step", "autonomous"
    });
    const bool mentions_files_or_code = contains_any(text, {
        ".cpp", ".hpp", ".h", ".cxx", ".cc", ".cmake", "cmakelists", "source",
        "function", "class", "method", "variable", "repo", "repository", "file",
        "files", "folder", "directory", "command", "terminal"
    });
    const bool simple_question = user_input.size() < 180 && contains_any(text, {
        "what is", "what's", "who is", "when is", "where is", "define ",
        "explain ", "summarize ", "quick", "simple"
    });

    if (likely_continuation || long_prompt || asks_for_steps || mentions_files_or_code)
        return ModelRoute::Deep;
    if (simple_question)
        return ModelRoute::Quick;
    return ModelRoute::Quick;
}

const std::string& Agent::model_for(ModelRoute route) const {
    if (route == ModelRoute::Deep && !deep_model_.empty()) return deep_model_;
    return quick_model_;
}

const char* Agent::route_name(ModelRoute route) const {
    return route == ModelRoute::Deep ? "deep" : "quick";
}

// ---------------------------------------------------------------------------
// History compaction
// ---------------------------------------------------------------------------

void Agent::trim_history() {
    // Estimate total serialized size
    size_t total = 0;
    for (auto& m : messages_) total += msg_size(m);

    if (total <= context_limit_) return;

    auto find_user_indices = [&]() {
        std::vector<size_t> idx;
        for (size_t i = 1; i < messages_.size(); ++i) {
            const auto& m = messages_[i];
            if (m.HasMember("role") && m["role"].IsString() &&
                std::string(m["role"].GetString()) == "user")
                idx.push_back(i);
        }
        return idx;
    };

    int trims = 0;
    while (total > context_limit_) {
        auto user_idx = find_user_indices();
        if (user_idx.size() <= MIN_TURNS_TO_KEEP) break;

        size_t cut_start = user_idx[0];
        size_t cut_end   = user_idx[1];

        size_t removed = 0;
        for (size_t i = cut_start; i < cut_end; ++i)
            removed += msg_size(messages_[i]);

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

Document Agent::chat(const std::vector<Document>& messages, ModelRoute route) {
    Document body;
    body.SetObject();
    auto& a = body.GetAllocator();

    // "messages" array
    Value msgs_arr(kArrayType);
    for (auto& m : messages) {
        Value copy;
        copy.CopyFrom(m, a);
        msgs_arr.PushBack(copy, a);
    }
    body.AddMember("messages", msgs_arr, a);

    // "tools" array — deep-copy from definitions() Document
    {
        Document defs = tools_.definitions();
        Value tools_val;
        tools_val.CopyFrom(defs, a);
        body.AddMember("tools", tools_val, a);
    }

    body.AddMember("tool_choice", Value("auto", a), a);
    body.AddMember("temperature", Value(0.2),       a);
    body.AddMember("max_tokens",  Value(4096),      a);

    const auto& selected_model = model_for(route);
    if (!selected_model.empty())
        body.AddMember("model", Value(selected_model.c_str(), a), a);

    std::string resp_str = http_post("/v1/chat/completions", body);

    Document resp = parse(resp_str);
    if (resp.HasMember("error") && resp["error"].IsObject()) {
        std::string msg = "unknown error";
        if (resp["error"].HasMember("message") && resp["error"]["message"].IsString())
            msg = resp["error"]["message"].GetString();
        throw std::runtime_error(msg);
    }
    return resp;
}

// ---------------------------------------------------------------------------
// HTTP helper
// ---------------------------------------------------------------------------

std::string Agent::http_post(const std::string& path, const Document& body) {
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

    std::string json_body = to_string(body);

    auto res = cli.Post(path, json_body, "application/json");
    if (!res)
        throw std::runtime_error("HTTP error: " + httplib::to_string(res.error()));
    if (res->status != 200)
        throw std::runtime_error("HTTP " + std::to_string(res->status) + ": " + res->body);

    return res->body;
}
