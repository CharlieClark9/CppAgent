#include "tools.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <array>
#include <regex>
#include <map>
#include <unordered_set>
#include <iostream>
#include <algorithm>
#include <vector>
#include <cctype>
#include <ranges>
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace fs = std::filesystem;
using namespace rapidjson;

static const std::vector<std::string> SKIP_DIRS = {
    ".git", "build", "out", "node_modules", "__pycache__", ".vs", ".cache", ".next",
    ".nuxt", ".svelte-kit", "coverage", ".turbo"
};

static const std::unordered_set<std::string> SKIP_EXTS = {
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".webp", ".bmp", ".tiff", ".avif",
    ".woff", ".woff2", ".ttf", ".otf", ".eot",
    ".exe", ".dll", ".so", ".dylib", ".obj", ".lib", ".pdb", ".ilk", ".exp",
    ".class", ".pyc", ".pyo",
    ".zip", ".gz", ".tar", ".rar", ".7z", ".bz2",
    ".mp3", ".mp4", ".wav", ".ogg", ".avi", ".mov",
    ".pdf", ".docx", ".xlsx", ".pptx",
    ".map",
};

static const std::unordered_set<std::string> SKIP_FILES = {
    "package-lock.json", "yarn.lock", "pnpm-lock.yaml",
    "Cargo.lock", "poetry.lock", "composer.lock", "Gemfile.lock",
};

Tools::Tools(std::string working_dir) : working_dir_(std::move(working_dir)) {}

// ---------------------------------------------------------------------------
// Tool schema definitions (OpenAI function-calling format) — rapidjson
// ---------------------------------------------------------------------------

// Helper: build a simple {"type":"string","description":"..."} property value.
static Value prop_str(const char* desc, Document::AllocatorType& a) {
    Value v(kObjectType);
    v.AddMember("type", Value("string", a), a);
    v.AddMember("description", Value(desc, a), a);
    return v;
}
static Value prop_int(const char* desc, Document::AllocatorType& a) {
    Value v(kObjectType);
    v.AddMember("type", Value("integer", a), a);
    v.AddMember("description", Value(desc, a), a);
    return v;
}

// Build a function-tool entry.
static Value make_tool(const char* name, const char* description,
                       Value& properties, Value& required,
                       Document::AllocatorType& a) {
    Value params(kObjectType);
    params.AddMember("type", Value("object", a), a);
    params.AddMember("properties", properties, a);
    params.AddMember("required", required, a);

    Value fn(kObjectType);
    fn.AddMember("name",        Value(name, a),        a);
    fn.AddMember("description", Value(description, a), a);
    fn.AddMember("parameters",  params,                a);

    Value tool(kObjectType);
    tool.AddMember("type",     Value("function", a), a);
    tool.AddMember("function", fn,                   a);
    return tool;
}

Document Tools::definitions() const {
    Document doc;
    doc.SetArray();
    auto& a = doc.GetAllocator();

    // ── list_files ──────────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("directory", prop_str("Directory to list (default: working directory)", a), a);
        props.AddMember("pattern",   prop_str("Legacy single include glob, e.g. '*.cpp'. Prefer include_globs.", a), a);
        props.AddMember("include_globs", prop_str(
            "Comma-separated path globs to include, e.g. '**/*.cpp,**/*.hpp,**/CMakeLists.txt'. "
            "Globs match paths relative to directory. Omit to include all files.", a), a);
        props.AddMember("exclude_globs", prop_str(
            "Comma-separated path globs to exclude, e.g. 'build/**,third_party/**,vendor/**'.", a), a);
        props.AddMember("limit", prop_int("Maximum number of files to return (default 500, max 2000).", a), a);
        props.AddMember("offset", prop_int("Number of matching files to skip for pagination (default 0).", a), a);
        Value req(kArrayType);
        doc.PushBack(make_tool("list_files",
            "List files in a directory tree. Supports path-aware include/exclude globs and pagination. "
            "Use this first to understand repo structure before reading anything. Skips common generated "
            "and dependency directories automatically.",
            props, req, a), a);
    }

    // ── read_file ────────────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("path", prop_str("File path to read (relative to working directory)", a), a);
        Value req(kArrayType);
        req.PushBack(Value("path", a), a);
        doc.PushBack(make_tool("read_file",
            "Read the full contents of a file. Use read_file_range instead for large files "
            "when you only need a specific section.",
            props, req, a), a);
    }

    // ── read_file_range ──────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("path",       prop_str("File path to read", a), a);
        props.AddMember("start_line", prop_int("First line to read (1-based)", a), a);
        props.AddMember("end_line",   prop_int("Last line to read (inclusive). Defaults to start_line + 99.", a), a);
        Value req(kArrayType);
        req.PushBack(Value("path", a), a);
        req.PushBack(Value("start_line", a), a);
        doc.PushBack(make_tool("read_file_range",
            "Read a specific line range from a file. Prefer this over read_file for large files. "
            "Lines are 1-based. Use search_files first to find the relevant line numbers.",
            props, req, a), a);
    }

    // ── write_file ───────────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("path",    prop_str("File path to write", a), a);
        props.AddMember("content", prop_str("Full file content to write", a), a);
        Value req(kArrayType);
        req.PushBack(Value("path", a), a);
        req.PushBack(Value("content", a), a);
        doc.PushBack(make_tool("write_file",
            "Write (or overwrite) a file with the given content. Creates parent directories as needed.",
            props, req, a), a);
    }

    // ── run_command ──────────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("cmd", prop_str("Command to execute", a), a);
        Value req(kArrayType);
        req.PushBack(Value("cmd", a), a);
        doc.PushBack(make_tool("run_command",
            "Run a shell command and return stdout + stderr. Use for building, testing, or any OS task.",
            props, req, a), a);
    }

    // ── search_files ─────────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("pattern",   prop_str("Regex pattern to search for", a), a);
        props.AddMember("directory", prop_str("Directory to search (default: working directory)", a), a);
        props.AddMember("file_glob", prop_str(
            "Legacy comma-separated include globs, e.g. '*.cpp,*.hpp'. Prefer include_globs.", a), a);
        props.AddMember("include_globs", prop_str(
            "Comma-separated path globs to include, e.g. '**/*.cpp,**/*.hpp,**/CMakeLists.txt'.", a), a);
        props.AddMember("exclude_globs", prop_str(
            "Comma-separated path globs to exclude, e.g. 'build/**,third_party/**,vendor/**'.", a), a);
        props.AddMember("limit", prop_int("Maximum number of matches to return (default 300, max 2000).", a), a);
        props.AddMember("offset", prop_int("Number of matching lines to skip for pagination (default 0).", a), a);
        Value req(kArrayType);
        req.PushBack(Value("pattern", a), a);
        doc.PushBack(make_tool("search_files",
            "Search for a regex pattern across files. Results are grouped by file. "
            "Use this to locate definitions, usages, or any string before reading files. "
            "On large repos, ALWAYS pass include_globs and exclude_globs to limit the search to relevant paths. "
            "Uses ripgrep automatically when installed.",
            props, req, a), a);
    }

    // ── edit_file ────────────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("path",     prop_str("File path to edit", a), a);
        props.AddMember("old_text", prop_str("Exact text to find and replace. Must be unique in the file.", a), a);
        props.AddMember("new_text", prop_str("Replacement text. May be empty to delete old_text.", a), a);
        Value req(kArrayType);
        req.PushBack(Value("path", a), a);
        req.PushBack(Value("old_text", a), a);
        req.PushBack(Value("new_text", a), a);
        doc.PushBack(make_tool("edit_file",
            "Replace an exact piece of text in a file. This is the PREFERRED way to edit existing files. "
            "old_text must match the file EXACTLY (including indentation and whitespace) and must be UNIQUE "
            "— if it appears zero times or more than once, the edit is rejected and nothing changes. "
            "Copy old_text directly from a recent read_file_range so it matches.",
            props, req, a), a);
    }

    // ── replace_block ────────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("path",         prop_str("File path to edit", a), a);
        props.AddMember("start_anchor", prop_str("Text identifying the first line of the block to replace", a), a);
        props.AddMember("end_anchor",   prop_str("Text identifying the last line of the block (first match at or after start)", a), a);
        props.AddMember("new_text",     prop_str("Replacement text for the whole block. May be empty to delete it.", a), a);
        Value req(kArrayType);
        req.PushBack(Value("path", a), a);
        req.PushBack(Value("start_anchor", a), a);
        req.PushBack(Value("end_anchor", a), a);
        req.PushBack(Value("new_text", a), a);
        doc.PushBack(make_tool("replace_block",
            "Replace a whole block of lines bounded by two text anchors (inclusive). Use this for removing or "
            "replacing large multi-line sections where copying exact text for edit_file would be error-prone.",
            props, req, a), a);
    }

    // ── finish_task ──────────────────────────────────────────────────────────
    {
        Value props(kObjectType);
        props.AddMember("summary", prop_str("Concise description of everything that was done", a), a);
        Value req(kArrayType);
        req.PushBack(Value("summary", a), a);
        doc.PushBack(make_tool("finish_task",
            "Signal that the current task is fully complete. Call this once all work is done "
            "and verified. The summary is shown to the user as the final result.",
            props, req, a), a);
    }

    return doc;
}

// ---------------------------------------------------------------------------
// Path helpers
// ---------------------------------------------------------------------------

std::string Tools::resolve(const std::string& path) const {
    fs::path p(path);
    if (p.is_absolute()) return path;
    return (fs::path(working_dir_) / p).string();
}

std::string Tools::relative(const std::string& abs_path) const {
    try {
        return fs::relative(abs_path, working_dir_).string();
    } catch (...) {
        return abs_path;
    }
}

// ---------------------------------------------------------------------------
// list_files
// ---------------------------------------------------------------------------

static bool should_skip_file(const fs::path& p) {
    std::string name = p.filename().string();
    if (SKIP_FILES.count(name)) return true;

    std::string ext = p.extension().string();
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (SKIP_EXTS.count(ext)) return true;

    if (name.size() > 7 && name.substr(name.size() - 7) == ".min.js")  return true;
    if (name.size() > 8 && name.substr(name.size() - 8) == ".min.css") return true;

    return false;
}

static std::string trim_copy(std::string s) {
    auto not_space = [](unsigned char c){ return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

static std::vector<std::string> split_csv(const std::string& csv) {
    std::vector<std::string> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        item = trim_copy(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

static std::string normalize_rel_path(std::string path) {
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0) path = path.substr(2);
    return path;
}

static bool has_slash(const std::string& s) {
    return s.find('/') != std::string::npos || s.find('\\') != std::string::npos;
}

static std::regex glob_to_regex(std::string glob) {
    glob = normalize_rel_path(trim_copy(glob));
    std::string re = "^";
    re.reserve(glob.size() * 3);

    for (size_t i = 0; i < glob.size(); ++i) {
        char c = glob[i];
        if (c == '*') {
            if (i + 1 < glob.size() && glob[i + 1] == '*') {
                i++;
                if (i + 1 < glob.size() && glob[i + 1] == '/') {
                    i++;
                    re += "(?:.*/)?";
                } else {
                    re += ".*";
                }
            } else {
                re += "[^/]*";
            }
        } else if (c == '?') {
            re += "[^/]";
        } else {
            if (std::string(".+^${}()|[]\\").find(c) != std::string::npos) re += "\\";
            re += c;
        }
    }

    re += "$";
    return std::regex(re, std::regex::icase);
}

static std::vector<std::regex> parse_globs(const std::string& csv) {
    std::vector<std::regex> out;
    for (auto item : split_csv(csv)) {
        try {
            out.push_back(glob_to_regex(item));
            if (!has_slash(item)) out.push_back(glob_to_regex("**/" + item));
        } catch (...) {}
    }
    return out;
}

static bool globs_match(const std::vector<std::regex>& globs, const std::string& rel) {
    for (const auto& g : globs)
        if (std::regex_match(rel, g)) return true;
    return false;
}

static bool path_allowed(const std::string& rel, const std::vector<std::regex>& include,
                         const std::vector<std::regex>& exclude) {
    if (!include.empty() && !globs_match(include, rel)) return false;
    if (!exclude.empty() && globs_match(exclude, rel)) return false;
    return true;
}

static int clamp_limit(int limit, int def) {
    if (limit <= 0) return def;
    return std::min(limit, 2000);
}

static int clamp_offset(int offset) {
    return std::max(offset, 0);
}

static int count_lines(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return 0;
    int lines = 0;
    char c;
    while (f.get(c)) if (c == '\n') ++lines;
    return lines;
}

ToolResult Tools::list_files(const std::string& dir, const std::string& include_globs,
                             const std::string& exclude_globs, int limit, int offset) {
    auto root = dir.empty() ? working_dir_ : resolve(dir);
    limit = clamp_limit(limit, 500);
    offset = clamp_offset(offset);

    if (ripgrep_available()) {
        bool ok = false;
        ToolResult r = list_files_with_ripgrep(root, include_globs, exclude_globs, limit, offset, ok);
        if (ok) return r;
    }
    return list_files_native(root, include_globs, exclude_globs, limit, offset);
}

// ---------------------------------------------------------------------------
// read_file
// ---------------------------------------------------------------------------

ToolResult Tools::read_file(const std::string& path) {
    auto full = resolve(path);
    std::ifstream f(full, std::ios::binary);
    if (!f) return {false, "Error: cannot open '" + full + "'"};

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    constexpr size_t MAX = 32'000;
    if (content.size() > MAX) {
        content.resize(MAX);
        content += "\n\n[...file truncated at 32 000 chars — use read_file_range for specific sections]";
    }
    return {true, content};
}

// ---------------------------------------------------------------------------
// read_file_range
// ---------------------------------------------------------------------------

ToolResult Tools::read_file_range(const std::string& path, int start_line, int end_line) {
    if (start_line < 1) start_line = 1;
    if (end_line < start_line) end_line = start_line + 99;
    constexpr int MAX_LINES = 500;
    if (end_line - start_line + 1 > MAX_LINES) end_line = start_line + MAX_LINES - 1;

    auto full = resolve(path);
    std::ifstream f(full);
    if (!f) return {false, "Error: cannot open '" + full + "'"};

    std::ostringstream out;
    std::string line;
    int current = 0;
    bool truncated = false;

    while (std::getline(f, line)) {
        ++current;
        if (current < start_line) continue;
        if (current > end_line) { truncated = true; break; }
        out << current << ": " << line << "\n";
    }

    if (current < start_line)
        return {false, "Error: file has only " + std::to_string(current) + " lines (requested start " + std::to_string(start_line) + ")"};

    std::string result = out.str();
    if (truncated)
        result += "[...truncated at line " + std::to_string(end_line) + "]";
    return {true, result};
}

// ---------------------------------------------------------------------------
// write_file
// ---------------------------------------------------------------------------

ToolResult Tools::write_file(const std::string& path, const std::string& content) {
    auto full = resolve(path);
    fs::create_directories(fs::path(full).parent_path());

    std::ofstream f(full, std::ios::binary | std::ios::trunc);
    if (!f) return {false, "Error: cannot write '" + full + "'"};

    f << content;
    return {true, "Wrote " + std::to_string(content.size()) + " bytes to " + relative(full)};
}

// ---------------------------------------------------------------------------
// run_command
// ---------------------------------------------------------------------------

ToolResult Tools::run_command(const std::string& cmd) {
    std::string output;
    std::array<char, 256> buf{};

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return {false, "Error: failed to run command"};

    while (fgets(buf.data(), (int)buf.size(), pipe))
        output += buf.data();

#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif

    constexpr size_t MAX = 16'000;
    bool truncated = output.size() > MAX;
    if (truncated) {
        output = output.substr(output.size() - MAX);
        output = "[...truncated, showing last 16 000 chars]\n" + output;
    }

    if (rc != 0) output += "\n[exit code: " + std::to_string(rc) + "]";
    return {true, output.empty() ? "(no output)" : output};
}

// ---------------------------------------------------------------------------
// search_files  (grouped by file) — ripgrep when available, native fallback
// ---------------------------------------------------------------------------

static constexpr int SEARCH_MAX_MATCHES = 300;

bool Tools::ripgrep_available() {
    if (rg_state_ >= 0) return rg_state_ == 1;
    std::string out;
    std::array<char, 128> buf{};
#ifdef _WIN32
    FILE* pipe = _popen("rg --version", "r");
#else
    FILE* pipe = popen("rg --version", "r");
#endif
    if (pipe) {
        while (fgets(buf.data(), (int)buf.size(), pipe)) out += buf.data();
#ifdef _WIN32
        int rc = _pclose(pipe);
#else
        int rc = pclose(pipe);
#endif
        rg_state_ = (rc == 0 && out.find("ripgrep") != std::string::npos) ? 1 : 0;
    } else {
        rg_state_ = 0;
    }
    return rg_state_ == 1;
}

static std::string shell_escape_quotes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    return out;
}

static void append_rg_globs(std::string& cmd, const std::string& include_globs,
                            const std::string& exclude_globs) {
    for (auto& g : split_csv(include_globs))
        cmd += " -g \"" + shell_escape_quotes(normalize_rel_path(g)) + "\"";
    for (auto& g : split_csv(exclude_globs))
        cmd += " -g \"!" + shell_escape_quotes(normalize_rel_path(g)) + "\"";
    for (auto& d : SKIP_DIRS)
        cmd += " -g \"!" + shell_escape_quotes(d) + "/**\"";
}

ToolResult Tools::list_files_with_ripgrep(const std::string& root, const std::string& include_globs,
                                          const std::string& exclude_globs, int limit, int offset,
                                          bool& ok) {
    ok = true;

#ifdef _WIN32
    std::string cmd = "cd /d \"" + root + "\" && rg --files --hidden --no-messages";
#else
    std::string cmd = "cd \"" + root + "\" && rg --files --hidden --no-messages";
#endif
    append_rg_globs(cmd, include_globs, exclude_globs);

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) { ok = false; return {false, "ripgrep launch failed"}; }

    std::ostringstream out;
    std::array<char, 4096> buf{};
    std::string leftover;
    int matched = 0;
    int shown = 0;

    auto handle_line = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string rel = normalize_rel_path(line);
        if (rel.empty()) return;
        if (should_skip_file(fs::path(rel))) return;
        ++matched;
        if (matched <= offset || shown >= limit) return;
        out << rel << " (" << count_lines(fs::path(root) / fs::path(rel)) << " lines)\n";
        ++shown;
    };

    while (fgets(buf.data(), (int)buf.size(), pipe)) {
        leftover += buf.data();
        size_t nl;
        while ((nl = leftover.find('\n')) != std::string::npos) {
            handle_line(leftover.substr(0, nl));
            leftover.erase(0, nl + 1);
        }
    }
    if (!leftover.empty()) handle_line(leftover);

#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif
    if (rc != 0 && matched == 0) { ok = false; return {false, "ripgrep error"}; }

    if (shown == 0) return {true, "No files found for the requested page/filter"};
    if (matched > offset + shown) out << "[...more files available; increase offset to continue]\n";
    out << "\n" << shown << " file(s) shown";
    if (offset > 0) out << " after offset " << offset;
    out << ", " << matched << " matching file(s) seen";
    return {true, out.str()};
}

ToolResult Tools::list_files_native(const std::string& root, const std::string& include_globs,
                                    const std::string& exclude_globs, int limit, int offset) {
    std::vector<std::regex> include = parse_globs(include_globs);
    std::vector<std::regex> exclude = parse_globs(exclude_globs);

    std::ostringstream out;
    int matched = 0;
    int shown = 0;

    try {
        for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied);
             it != fs::end(it); ++it)
        {
            if (it->is_directory()) {
                auto name = it->path().filename().string();
                if (std::ranges::contains(SKIP_DIRS, name))
                    it.disable_recursion_pending();
                continue;
            }

            if (!it->is_regular_file()) continue;
            if (should_skip_file(it->path())) continue;

            std::string rel = normalize_rel_path(relative(it->path().string()));
            if (!path_allowed(rel, include, exclude)) continue;

            ++matched;
            if (matched <= offset || shown >= limit) continue;

            out << rel << " (" << count_lines(it->path()) << " lines)\n";
            ++shown;
        }
    } catch (const fs::filesystem_error& e) {
        return {false, std::string("Filesystem error: ") + e.what()};
    }

    if (shown == 0) return {true, "No files found for the requested page/filter"};
    if (matched > offset + shown) out << "[...more files available; increase offset to continue]\n";
    out << "\n" << shown << " file(s) shown";
    if (offset > 0) out << " after offset " << offset;
    out << ", " << matched << " matching file(s)";
    return {true, out.str()};
}

ToolResult Tools::search_with_ripgrep(const std::string& pattern, const std::string& root,
                                      const std::string& include_globs, const std::string& exclude_globs,
                                      int limit, int offset, bool& ok) {
    ok = true;

    std::string esc;
    for (char c : pattern) { if (c == '"') esc += "\\\""; else esc += c; }

#ifdef _WIN32
    std::string cmd = "cd /d \"" + root + "\" && ";
#else
    std::string cmd = "cd \"" + root + "\" && ";
#endif
    cmd += "rg --no-heading --line-number --color never --ignore-case --no-messages";
    append_rg_globs(cmd, include_globs, exclude_globs);
    cmd += " -e \"" + esc + "\" .";

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) { ok = false; return {false, "ripgrep launch failed"}; }

    std::map<std::string, std::vector<std::pair<int,std::string>>> grouped;
    int matched = 0;
    int shown = 0;
    bool capped = false;
    std::array<char, 4096> buf{};
    std::string leftover;

    auto handle_line = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t c1 = line.find(':');
        if (c1 == std::string::npos) return;
        size_t c2 = line.find(':', c1 + 1);
        if (c2 == std::string::npos) return;
        std::string path = line.substr(0, c1);
        std::string lnum = line.substr(c1 + 1, c2 - c1 - 1);
        std::string text = line.substr(c2 + 1);
        if (path.rfind("./", 0) == 0 || path.rfind(".\\", 0) == 0) path = path.substr(2);
        int ln;
        try { ln = std::stoi(lnum); } catch (...) { return; }
        if (text.size() > 200) text = text.substr(0, 200) + "...";
        ++matched;
        if (matched <= offset) return;
        if (shown >= limit) { capped = true; return; }
        grouped[path].emplace_back(ln, std::move(text));
        ++shown;
    };

    while (!capped && fgets(buf.data(), (int)buf.size(), pipe)) {
        leftover += buf.data();
        size_t nl;
        while ((nl = leftover.find('\n')) != std::string::npos) {
            handle_line(leftover.substr(0, nl));
            leftover.erase(0, nl + 1);
            if (shown >= limit) { capped = true; break; }
        }
    }

#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif

    if (shown == 0) {
        if (rc == 2) { ok = false; return {false, "ripgrep error"}; }
        return {true, "No matches found for '" + pattern + "' on the requested page/filter"};
    }

    std::ostringstream out;
    for (auto& [file, matches] : grouped) {
        out << file << " (" << matches.size() << " match" << (matches.size() > 1 ? "es" : "") << "):\n";
        for (auto& [l, t] : matches) out << "  " << l << ": " << t << "\n";
        out << "\n";
    }
    if (capped) out << "[...more matches available; increase offset to continue]\n";
    out << grouped.size() << " file(s), " << shown << " match(es) shown";
    if (offset > 0) out << " after offset " << offset;
    return {true, out.str()};
}

ToolResult Tools::search_native(const std::string& pattern, const std::string& root,
                                const std::string& include_globs, const std::string& exclude_globs,
                                int limit, int offset) {
    std::regex re;
    try {
        re = std::regex(pattern, std::regex::icase);
    } catch (...) {
        return {false, "Error: invalid regex '" + pattern + "'"};
    }

    std::vector<std::regex> include = parse_globs(include_globs);
    std::vector<std::regex> exclude = parse_globs(exclude_globs);

    std::map<std::string, std::vector<std::pair<int,std::string>>> grouped;
    int matched = 0;
    int shown = 0;

    try {
        for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied);
             it != fs::end(it) && shown < limit; ++it)
        {
            if (it->is_directory()) {
                if (std::ranges::contains(SKIP_DIRS, it->path().filename().string()))
                    it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            if (should_skip_file(it->path())) continue;
            std::string rel = normalize_rel_path(relative(it->path().string()));
            if (!path_allowed(rel, include, exclude)) continue;

            std::ifstream f(it->path(), std::ios::binary);
            if (!f) continue;

            int line_num = 0;
            std::string line;
            std::vector<std::pair<int,std::string>> file_matches;

            while (std::getline(f, line) && shown < limit) {
                ++line_num;
                if (std::regex_search(line, re)) {
                    ++matched;
                    if (matched <= offset) continue;
                    std::string display = line.size() > 200 ? line.substr(0, 200) + "..." : line;
                    file_matches.emplace_back(line_num, std::move(display));
                    ++shown;
                }
            }

            if (!file_matches.empty())
                grouped[rel] = std::move(file_matches);
        }
    } catch (const fs::filesystem_error& e) {
        return {false, std::string("Filesystem error: ") + e.what()};
    }

    if (grouped.empty()) return {true, "No matches found for '" + pattern + "'"};

    std::ostringstream out;
    for (auto& [file, matches] : grouped) {
        out << file << " (" << matches.size() << " match" << (matches.size() > 1 ? "es" : "") << "):\n";
        for (auto& [ln, text] : matches)
            out << "  " << ln << ": " << text << "\n";
        out << "\n";
    }

    if (shown >= limit)
        out << "[...more matches may be available; increase offset to continue]\n";

    out << grouped.size() << " file(s), " << shown << " match(es) shown";
    if (offset > 0) out << " after offset " << offset;
    return {true, out.str()};
}

ToolResult Tools::search_files(const std::string& pattern, const std::string& dir,
                               const std::string& include_globs, const std::string& exclude_globs,
                               int limit, int offset) {
    auto search_root = dir.empty() ? working_dir_ : resolve(dir);
    limit = clamp_limit(limit, SEARCH_MAX_MATCHES);
    offset = clamp_offset(offset);

    if (ripgrep_available()) {
        bool ok = false;
        ToolResult r = search_with_ripgrep(pattern, search_root, include_globs, exclude_globs,
                                           limit, offset, ok);
        if (ok) return r;
    }
    return search_native(pattern, search_root, include_globs, exclude_globs, limit, offset);
}

// ---------------------------------------------------------------------------
// File-edit helpers
// ---------------------------------------------------------------------------

static bool read_normalized(const std::string& full, std::string& out) {
    std::ifstream f(full, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r') {
            if (i + 1 < out.size() && out[i + 1] == '\n') continue;
            norm += '\n';
        } else {
            norm += out[i];
        }
    }
    out.swap(norm);
    return true;
}

static bool write_all(const std::string& full, const std::string& content) {
    std::ofstream out(full, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << content;
    return true;
}

static size_t count_occurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    size_t count = 0, pos = 0;
    while ((pos = hay.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

// ---------------------------------------------------------------------------
// edit_file
// ---------------------------------------------------------------------------

ToolResult Tools::edit_file(const std::string& path, const std::string& old_text,
                            const std::string& new_text) {
    auto full = resolve(path);

    if (old_text.empty())
        return {false, "Error: old_text must not be empty. Provide the exact text to replace."};

    std::string content;
    if (!read_normalized(full, content))
        return {false, "Error: cannot open '" + full + "'"};

    std::string needle;
    needle.reserve(old_text.size());
    for (size_t i = 0; i < old_text.size(); ++i) {
        if (old_text[i] == '\r') {
            if (i + 1 < old_text.size() && old_text[i + 1] == '\n') continue;
            needle += '\n';
        } else {
            needle += old_text[i];
        }
    }

    size_t matches = count_occurrences(content, needle);
    if (matches == 0)
        return {false,
            "Error: old_text not found in " + relative(full) + ".\n"
            "The text must match EXACTLY, including indentation and whitespace. "
            "Use read_file_range to copy the exact text, then try again."};
    if (matches > 1)
        return {false,
            "Error: old_text appears " + std::to_string(matches) + " times in " + relative(full) +
            " — it is ambiguous.\nAdd more surrounding lines to old_text so it matches exactly ONE location."};

    size_t pos = content.find(needle);
    std::string normalized_new;
    normalized_new.reserve(new_text.size());
    for (size_t i = 0; i < new_text.size(); ++i) {
        if (new_text[i] == '\r') {
            if (i + 1 < new_text.size() && new_text[i + 1] == '\n') continue;
            normalized_new += '\n';
        } else {
            normalized_new += new_text[i];
        }
    }

    content.replace(pos, needle.size(), normalized_new);
    if (!write_all(full, content))
        return {false, "Error: cannot write '" + full + "'"};

    auto count_lines = [](const std::string& s) {
        return s.empty() ? 0 : (int)std::count(s.begin(), s.end(), '\n') + 1;
    };
    return {true, "Edited " + relative(full) + ": replaced " + std::to_string(count_lines(needle)) +
                  " line(s) with " + std::to_string(count_lines(normalized_new)) + " line(s)."};
}

// ---------------------------------------------------------------------------
// replace_block
// ---------------------------------------------------------------------------

ToolResult Tools::replace_block(const std::string& path, const std::string& start_anchor,
                                const std::string& end_anchor, const std::string& new_text) {
    auto full = resolve(path);

    if (start_anchor.empty() || end_anchor.empty())
        return {false, "Error: both start_anchor and end_anchor are required."};

    std::string content;
    if (!read_normalized(full, content))
        return {false, "Error: cannot open '" + full + "'"};

    std::vector<std::string> lines;
    {
        std::istringstream ss(content);
        std::string l;
        while (std::getline(ss, l)) lines.push_back(l);
    }

    int start_idx = -1;
    for (int i = 0; i < (int)lines.size(); ++i)
        if (lines[i].find(start_anchor) != std::string::npos) { start_idx = i; break; }
    if (start_idx < 0)
        return {false, "Error: start_anchor not found: \"" + start_anchor + "\""};

    int end_idx = -1;
    for (int i = start_idx; i < (int)lines.size(); ++i)
        if (lines[i].find(end_anchor) != std::string::npos) { end_idx = i; break; }
    if (end_idx < 0)
        return {false, "Error: end_anchor \"" + end_anchor +
                       "\" not found at or after start_anchor (line " + std::to_string(start_idx + 1) + ")."};

    std::ostringstream removed;
    for (int i = start_idx; i <= end_idx; ++i)
        removed << "  " << (i + 1) << ": " << lines[i] << "\n";

    std::vector<std::string> replacement;
    if (!new_text.empty()) {
        std::istringstream ss(new_text);
        std::string l;
        while (std::getline(ss, l)) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            replacement.push_back(l);
        }
    }

    lines.erase(lines.begin() + start_idx, lines.begin() + end_idx + 1);
    lines.insert(lines.begin() + start_idx, replacement.begin(), replacement.end());

    std::string rebuilt;
    for (size_t i = 0; i < lines.size(); ++i) {
        rebuilt += lines[i];
        if (i + 1 < lines.size()) rebuilt += '\n';
    }
    rebuilt += '\n';

    if (!write_all(full, rebuilt))
        return {false, "Error: cannot write '" + full + "'"};

    int removed_count = end_idx - start_idx + 1;
    return {true, "Replaced block in " + relative(full) + ": lines " + std::to_string(start_idx + 1) +
                  "-" + std::to_string(end_idx + 1) + " (" + std::to_string(removed_count) +
                  " line(s)) with " + std::to_string((int)replacement.size()) + " line(s).\n"
                  "Removed:\n" + removed.str()};
}

// ---------------------------------------------------------------------------
// finish_task
// ---------------------------------------------------------------------------

ToolResult Tools::finish_task(const std::string& summary) {
    task_done_    = true;
    task_summary_ = summary;
    return {true, "Task marked complete."};
}

// ---------------------------------------------------------------------------
// Dispatch — reads args from a rapidjson Value object
// ---------------------------------------------------------------------------

ToolResult Tools::dispatch(const std::string& name, const rapidjson::Value& args) {
    // Helper: get string arg or default
    auto str = [&](const char* key, const char* def = "") -> std::string {
        auto it = args.FindMember(key);
        if (it == args.MemberEnd() || !it->value.IsString()) return def;
        return it->value.GetString();
    };
    // Helper: get int arg or default
    auto num = [&](const char* key, int def = 0) -> int {
        auto it = args.FindMember(key);
        if (it == args.MemberEnd()) return def;
        if (it->value.IsInt())    return it->value.GetInt();
        if (it->value.IsDouble()) return (int)it->value.GetDouble();
        if (it->value.IsString()) {
            try { return std::stoi(it->value.GetString()); } catch (...) {}
        }
        return def;
    };
    auto search_include_globs = [&]() -> std::string {
        std::string inc = str("include_globs");
        if (inc.empty()) inc = str("file_glob");
        return inc;
    };
    auto list_include_globs = [&]() -> std::string {
        std::string inc = search_include_globs();
        if (inc.empty()) inc = str("pattern");
        return inc;
    };

    if (name == "list_files")
        return list_files(str("directory"), list_include_globs(), str("exclude_globs"),
                          num("limit", 500), num("offset", 0));
    if (name == "read_file")
        return read_file(str("path"));
    if (name == "read_file_range")
        return read_file_range(str("path"), num("start_line", 1), num("end_line", 0));
    if (name == "write_file")
        return write_file(str("path"), str("content"));
    if (name == "run_command")
        return run_command(str("cmd"));
    if (name == "search_files")
        return search_files(str("pattern"), str("directory"), search_include_globs(), str("exclude_globs"),
                            num("limit", SEARCH_MAX_MATCHES), num("offset", 0));
    if (name == "edit_file")
        return edit_file(str("path"), str("old_text"), str("new_text"));
    if (name == "replace_block")
        return replace_block(str("path"), str("start_anchor"), str("end_anchor"), str("new_text"));
    if (name == "finish_task")
        return finish_task(str("summary"));

    return {false, "Unknown tool: " + name};
}
