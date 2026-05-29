#pragma once
#include <nlohmann/json.hpp>
#include <string>

struct ToolResult {
    bool success;
    std::string output;
};

class Tools {
public:
    explicit Tools(std::string working_dir);

    nlohmann::json definitions() const;
    ToolResult dispatch(const std::string& name, const nlohmann::json& args);

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

    ToolResult list_files(const std::string& dir, const std::string& pattern);
    ToolResult read_file(const std::string& path);
    ToolResult read_file_range(const std::string& path, int start_line, int end_line);
    ToolResult write_file(const std::string& path, const std::string& content);
    ToolResult edit_file(const std::string& path, const std::string& old_text, const std::string& new_text);
    ToolResult replace_block(const std::string& path, const std::string& start_anchor,
                             const std::string& end_anchor, const std::string& new_text);
    ToolResult run_command(const std::string& cmd);
    ToolResult search_files(const std::string& pattern, const std::string& dir, const std::string& file_glob);
    ToolResult finish_task(const std::string& summary);

    // ripgrep acceleration
    bool ripgrep_available();
    ToolResult search_with_ripgrep(const std::string& pattern, const std::string& root,
                                   const std::string& file_glob, bool& ok);
    ToolResult search_native(const std::string& pattern, const std::string& root,
                             const std::string& file_glob);
    int  rg_state_ = -1;   // -1 = not yet probed, 0 = unavailable, 1 = available
};
