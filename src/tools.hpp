#pragma once
#include <rapidjson/document.h>
#include <rapidjson/allocators.h>
#include <string>

struct ToolResult {
    bool success;
    std::string output;
};

class Tools {
public:
    explicit Tools(std::string working_dir);

    // Returns a rapidjson Document (array) describing all tool schemas.
    rapidjson::Document definitions() const;

    // args is a parsed JSON object from the LLM tool-call arguments.
    ToolResult dispatch(const std::string& name, const rapidjson::Value& args);

    void set_working_dir(std::string dir) { working_dir_ = std::move(dir); }
    const std::string& get_working_dir() const { return working_dir_; }

    bool        task_done()    const { return task_done_; }
    std::string task_summary() const { return task_summary_; }
    void        reset_task()         { task_done_ = false; task_summary_.clear(); }

private:
    std::string working_dir_;
    bool        task_done_    = false;
    std::string task_summary_;

    std::string resolve(const std::string& path) const;
    std::string relative(const std::string& abs_path) const;

    ToolResult list_files(const std::string& dir, const std::string& include_globs,
                          const std::string& exclude_globs, int limit, int offset);
    ToolResult read_file(const std::string& path);
    ToolResult read_file_range(const std::string& path, int start_line, int end_line);
    ToolResult write_file(const std::string& path, const std::string& content);
    ToolResult edit_file(const std::string& path, const std::string& old_text, const std::string& new_text);
    ToolResult replace_block(const std::string& path, const std::string& start_anchor,
                             const std::string& end_anchor, const std::string& new_text);
    ToolResult run_command(const std::string& cmd);
    ToolResult search_files(const std::string& pattern, const std::string& dir,
                            const std::string& include_globs, const std::string& exclude_globs,
                            int limit, int offset);
    ToolResult finish_task(const std::string& summary);

    // ripgrep acceleration
    bool ripgrep_available();
    ToolResult list_files_with_ripgrep(const std::string& root, const std::string& include_globs,
                                       const std::string& exclude_globs, int limit, int offset,
                                       bool& ok);
    ToolResult list_files_native(const std::string& root, const std::string& include_globs,
                                 const std::string& exclude_globs, int limit, int offset);
    ToolResult search_with_ripgrep(const std::string& pattern, const std::string& root,
                                   const std::string& include_globs, const std::string& exclude_globs,
                                   int limit, int offset, bool& ok);
    ToolResult search_native(const std::string& pattern, const std::string& root,
                             const std::string& include_globs, const std::string& exclude_globs,
                             int limit, int offset);
    int  rg_state_ = -1;   // -1 = not yet probed, 0 = unavailable, 1 = available
};
