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

namespace fs = std::filesystem;

static const std::vector<std::string> SKIP_DIRS = {
    ".git", "build", "out", "node_modules", "__pycache__", ".vs", ".cache", ".next",
    ".nuxt", ".svelte-kit", "coverage", ".turbo"
};

// Binary or generated files that are never useful to read or search.
static const std::unordered_set<std::string> SKIP_EXTS = {
    // images
    ".png", ".jpg", ".jpeg", ".gif", ".ico", ".webp", ".bmp", ".tiff", ".avif",
    // fonts
    ".woff", ".woff2", ".ttf", ".otf", ".eot",
    // compiled / binary
    ".exe", ".dll", ".so", ".dylib", ".obj", ".lib", ".pdb", ".ilk", ".exp",
    ".class", ".pyc", ".pyo",
    // archives
    ".zip", ".gz", ".tar", ".rar", ".7z", ".bz2",
    // media
    ".mp3", ".mp4", ".wav", ".ogg", ".avi", ".mov",
    // documents
    ".pdf", ".docx", ".xlsx", ".pptx",
    // misc large generated
    ".map",      // JS source maps — huge, machine-generated
    ".min.js",   // won't match via extension alone, handled below
};

// Specific filenames that are valid text but too large / low-signal to search.
static const std::unordered_set<std::string> SKIP_FILES = {
    "package-lock.json", "yarn.lock", "pnpm-lock.yaml",
    "Cargo.lock", "poetry.lock", "composer.lock", "Gemfile.lock",
};

Tools::Tools(std::string working_dir) : working_dir_(std::move(working_dir)) {}

// ---------------------------------------------------------------------------
// Tool schema definitions (OpenAI function-calling format)
// ---------------------------------------------------------------------------

nlohmann::json Tools::definitions() const {
    return nlohmann::json::array({
        {
            {"type", "function"},
            {"function", {
                {"name", "list_files"},
                {"description",
                    "List files in a directory tree. Use this first to understand repo structure "
                    "before reading anything. Skips .git, build, node_modules automatically."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"directory", {{"type", "string"}, {"description", "Directory to list (default: working directory)"}}},
                        {"pattern",   {{"type", "string"}, {"description", "Optional glob pattern, e.g. '*.cpp', '*.hpp'. Omit to list all files."}}}
                    }},
                    {"required", nlohmann::json::array()}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "read_file"},
                {"description",
                    "Read the full contents of a file. Use read_file_range instead for large files "
                    "when you only need a specific section."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path", {{"type", "string"}, {"description", "File path to read (relative to working directory)"}}}
                    }},
                    {"required", {"path"}}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "read_file_range"},
                {"description",
                    "Read a specific line range from a file. Prefer this over read_file for large files. "
                    "Lines are 1-based. Use search_files first to find the relevant line numbers."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path",       {{"type", "string"},  {"description", "File path to read"}}},
                        {"start_line", {{"type", "integer"}, {"description", "First line to read (1-based)"}}},
                        {"end_line",   {{"type", "integer"}, {"description", "Last line to read (inclusive). Defaults to start_line + 99."}}}
                    }},
                    {"required", {"path", "start_line"}}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "write_file"},
                {"description", "Write (or overwrite) a file with the given content. Creates parent directories as needed."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path",    {{"type", "string"}, {"description", "File path to write"}}},
                        {"content", {{"type", "string"}, {"description", "Full file content to write"}}}
                    }},
                    {"required", {"path", "content"}}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "run_command"},
                {"description", "Run a shell command and return stdout + stderr. Use for building, testing, or any OS task."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"cmd", {{"type", "string"}, {"description", "Command to execute"}}}
                    }},
                    {"required", {"cmd"}}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "search_files"},
                {"description",
                    "Search for a regex pattern across files. Results are grouped by file. "
                    "Use this to locate definitions, usages, or any string before reading files. "
                    "On large repos, ALWAYS pass file_glob to limit the search to relevant file types "
                    "(e.g. '*.cpp,*.hpp' or '*.ts,*.tsx') — this is faster and cuts noise. "
                    "Uses ripgrep automatically when installed."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"pattern",   {{"type", "string"}, {"description", "Regex pattern to search for"}}},
                        {"directory", {{"type", "string"}, {"description", "Directory to search (default: working directory)"}}},
                        {"file_glob", {{"type", "string"}, {"description", "Optional comma-separated file globs to restrict the search, e.g. '*.cpp,*.hpp'. Omit to search all text files."}}}
                    }},
                    {"required", {"pattern"}}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "edit_file"},
                {"description",
                    "Replace an exact piece of text in a file. This is the PREFERRED way to edit existing files. "
                    "old_text must match the file EXACTLY (including indentation and whitespace) and must be UNIQUE "
                    "— if it appears zero times or more than once, the edit is rejected and nothing changes. "
                    "Copy old_text directly from a recent read_file_range so it matches. Include a few surrounding "
                    "lines if needed to make it unique. No line numbers are used, so this is safe against line drift."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path",     {{"type", "string"}, {"description", "File path to edit"}}},
                        {"old_text", {{"type", "string"}, {"description", "Exact text to find and replace. Must be unique in the file."}}},
                        {"new_text", {{"type", "string"}, {"description", "Replacement text. May be empty to delete old_text."}}}
                    }},
                    {"required", {"path", "old_text", "new_text"}}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "replace_block"},
                {"description",
                    "Replace a whole block of lines bounded by two text anchors (inclusive). Use this for removing or "
                    "replacing large multi-line sections (e.g. an entire <header>...</header> block) where copying "
                    "the exact text for edit_file would be error-prone. The tool finds the first line containing "
                    "start_anchor, then the first line at or after it containing end_anchor, and replaces everything "
                    "between them. Choose distinctive anchors (e.g. '<header' and '</header>'), not generic ones like '</div>'."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"path",         {{"type", "string"}, {"description", "File path to edit"}}},
                        {"start_anchor", {{"type", "string"}, {"description", "Text identifying the first line of the block to replace"}}},
                        {"end_anchor",   {{"type", "string"}, {"description", "Text identifying the last line of the block (first match at or after start)"}}},
                        {"new_text",     {{"type", "string"}, {"description", "Replacement text for the whole block. May be empty to delete it."}}}
                    }},
                    {"required", {"path", "start_anchor", "end_anchor", "new_text"}}
                }}
            }}
        },
        {
            {"type", "function"},
            {"function", {
                {"name", "finish_task"},
                {"description",
                    "Signal that the current task is fully complete. Call this once all work is done "
                    "and verified. The summary is shown to the user as the final result."},
                {"parameters", {
                    {"type", "object"},
                    {"properties", {
                        {"summary", {{"type", "string"}, {"description", "Concise description of everything that was done"}}}
                    }},
                    {"required", {"summary"}}
                }}
            }}
        }
    });
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

// Returns true if this file should be skipped entirely.
static bool should_skip_file(const fs::path& p) {
    std::string name = p.filename().string();
    if (SKIP_FILES.count(name)) return true;

    std::string ext = p.extension().string();
    // Lowercase the extension for case-insensitive matching
    for (char& c : ext) c = (char)std::tolower((unsigned char)c);
    if (SKIP_EXTS.count(ext)) return true;

    // Catch *.min.js, *.min.css (two-part "extension")
    if (name.size() > 7 && name.substr(name.size() - 7) == ".min.js")  return true;
    if (name.size() > 8 && name.substr(name.size() - 8) == ".min.css") return true;

    return false;
}

// Convert a simple glob pattern (*.cpp, *.hpp) to a regex.
static std::regex glob_to_regex(const std::string& glob) {
    std::string re;
    re.reserve(glob.size() * 2);
    for (char c : glob) {
        switch (c) {
            case '*': re += ".*"; break;
            case '?': re += ".";  break;
            case '.': re += "\\."; break;
            case '+': re += "\\+"; break;
            case '^': re += "\\^"; break;
            case '$': re += "\\$"; break;
            default:  re += c;    break;
        }
    }
    return std::regex(re, std::regex::icase);
}

ToolResult Tools::list_files(const std::string& dir, const std::string& pattern) {
    auto root = dir.empty() ? working_dir_ : resolve(dir);

    std::optional<std::regex> filter;
    if (!pattern.empty()) {
        try { filter = glob_to_regex(pattern); }
        catch (...) { return {false, "Error: invalid pattern '" + pattern + "'"}; }
    }

    std::ostringstream out;
    int count = 0;
    constexpr int MAX_FILES = 500;

    try {
        for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied);
             it != fs::end(it) && count < MAX_FILES; ++it)
        {
            if (it->is_directory()) {
                auto name = it->path().filename().string();
                if (std::ranges::contains(SKIP_DIRS, name))
                    it.disable_recursion_pending();
                continue;
            }

            if (!it->is_regular_file()) continue;
            if (should_skip_file(it->path())) continue;

            std::string filename = it->path().filename().string();
            if (filter && !std::regex_match(filename, *filter)) continue;

            // Count lines cheaply
            std::ifstream f(it->path(), std::ios::binary);
            int lines = 0;
            if (f) {
                char c;
                while (f.get(c)) if (c == '\n') ++lines;
            }

            std::string rel = relative(it->path().string());
            out << rel << " (" << lines << " lines)\n";
            ++count;
        }
    } catch (const fs::filesystem_error& e) {
        return {false, std::string("Filesystem error: ") + e.what()};
    }

    if (count == 0) return {true, "No files found" + (pattern.empty() ? "" : " matching '" + pattern + "'")};
    if (count >= MAX_FILES) out << "[...truncated at " << MAX_FILES << " files]\n";
    out << "\n" << count << " file(s)";
    return {true, out.str()};
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
        // Keep the tail — compiler errors appear at the end
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

// Parse a comma-separated glob list ("*.cpp, *.hpp") into filename regexes.
static std::vector<std::regex> parse_globs(const std::string& csv) {
    std::vector<std::regex> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto a = item.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        auto b = item.find_last_not_of(" \t");
        item = item.substr(a, b - a + 1);
        try { out.push_back(glob_to_regex(item)); } catch (...) {}
    }
    return out;
}

// Lazily probe (once) whether `rg` is on PATH.
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

ToolResult Tools::search_with_ripgrep(const std::string& pattern, const std::string& root,
                                      const std::string& file_glob, bool& ok) {
    ok = true;

    // Escape the pattern for cmd/sh double-quoting (regex backslashes pass through).
    std::string esc;
    for (char c : pattern) { if (c == '"') esc += "\\\""; else esc += c; }

#ifdef _WIN32
    std::string cmd = "cd /d \"" + root + "\" && ";
#else
    std::string cmd = "cd \"" + root + "\" && ";
#endif
    cmd += "rg --no-heading --line-number --color never --ignore-case --no-messages";

    {
        std::stringstream ss(file_glob);
        std::string g;
        while (std::getline(ss, g, ',')) {
            auto a = g.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            auto b = g.find_last_not_of(" \t");
            g = g.substr(a, b - a + 1);
            cmd += " -g \"" + g + "\"";
        }
    }
    for (auto& d : SKIP_DIRS) cmd += " -g \"!" + d + "/\"";
    cmd += " -e \"" + esc + "\" .";

#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) { ok = false; return {false, "ripgrep launch failed"}; }

    std::map<std::string, std::vector<std::pair<int,std::string>>> grouped;
    int total = 0;
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
        grouped[path].emplace_back(ln, std::move(text));
        ++total;
    };

    while (!capped && fgets(buf.data(), (int)buf.size(), pipe)) {
        leftover += buf.data();
        size_t nl;
        while ((nl = leftover.find('\n')) != std::string::npos) {
            handle_line(leftover.substr(0, nl));
            leftover.erase(0, nl + 1);
            if (total >= SEARCH_MAX_MATCHES) { capped = true; break; }
        }
    }

#ifdef _WIN32
    int rc = _pclose(pipe);
#else
    int rc = pclose(pipe);
#endif

    if (total == 0) {
        if (rc == 2) { ok = false; return {false, "ripgrep error"}; }  // bad regex etc. -> fall back
        return {true, "No matches found for '" + pattern + "'"};
    }

    std::ostringstream out;
    for (auto& [file, matches] : grouped) {
        out << file << " (" << matches.size() << " match" << (matches.size() > 1 ? "es" : "") << "):\n";
        for (auto& [l, t] : matches) out << "  " << l << ": " << t << "\n";
        out << "\n";
    }
    if (capped) out << "[...truncated at " << SEARCH_MAX_MATCHES << " total matches]\n";
    out << grouped.size() << " file(s), " << total << " match(es)";
    return {true, out.str()};
}

ToolResult Tools::search_native(const std::string& pattern, const std::string& root,
                                const std::string& file_glob) {
    std::regex re;
    try {
        re = std::regex(pattern, std::regex::icase);
    } catch (...) {
        return {false, "Error: invalid regex '" + pattern + "'"};
    }

    std::vector<std::regex> globs = parse_globs(file_glob);
    auto name_ok = [&](const std::string& fn) {
        if (globs.empty()) return true;
        for (auto& g : globs) if (std::regex_match(fn, g)) return true;
        return false;
    };

    std::map<std::string, std::vector<std::pair<int,std::string>>> grouped;
    int total_matches = 0;

    try {
        for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied);
             it != fs::end(it) && total_matches < SEARCH_MAX_MATCHES; ++it)
        {
            if (it->is_directory()) {
                if (std::ranges::contains(SKIP_DIRS, it->path().filename().string()))
                    it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file()) continue;
            if (should_skip_file(it->path())) continue;
            if (!name_ok(it->path().filename().string())) continue;

            std::ifstream f(it->path(), std::ios::binary);
            if (!f) continue;

            int line_num = 0;
            std::string line;
            std::vector<std::pair<int,std::string>> file_matches;

            while (std::getline(f, line) && total_matches < SEARCH_MAX_MATCHES) {
                ++line_num;
                if (std::regex_search(line, re)) {
                    std::string display = line.size() > 200 ? line.substr(0, 200) + "..." : line;
                    file_matches.emplace_back(line_num, std::move(display));
                    ++total_matches;
                }
            }

            if (!file_matches.empty())
                grouped[relative(it->path().string())] = std::move(file_matches);
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

    if (total_matches >= SEARCH_MAX_MATCHES)
        out << "[...truncated at " << SEARCH_MAX_MATCHES << " total matches]\n";

    out << grouped.size() << " file(s), " << total_matches << " match(es)";
    return {true, out.str()};
}

ToolResult Tools::search_files(const std::string& pattern, const std::string& dir,
                               const std::string& file_glob) {
    auto search_root = dir.empty() ? working_dir_ : resolve(dir);

    if (ripgrep_available()) {
        bool ok = false;
        ToolResult r = search_with_ripgrep(pattern, search_root, file_glob, ok);
        if (ok) return r;
        // ripgrep errored (e.g. dialect mismatch) — fall back to the native walk
    }
    return search_native(pattern, search_root, file_glob);
}

// ---------------------------------------------------------------------------
// File-edit helpers
// ---------------------------------------------------------------------------

// Read a whole file, normalising CRLF/CR line endings to LF so matching is
// independent of the file's on-disk line endings. Returns false if unreadable.
static bool read_normalized(const std::string& full, std::string& out) {
    std::ifstream f(full, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    // CRLF -> LF
    std::string norm;
    norm.reserve(out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i] == '\r') {
            if (i + 1 < out.size() && out[i + 1] == '\n') continue; // drop CR of CRLF
            norm += '\n';                                           // lone CR -> LF
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
// edit_file  (exact string replacement with a uniqueness guarantee)
// ---------------------------------------------------------------------------

ToolResult Tools::edit_file(const std::string& path, const std::string& old_text,
                            const std::string& new_text) {
    auto full = resolve(path);

    if (old_text.empty())
        return {false, "Error: old_text must not be empty. Provide the exact text to replace."};

    std::string content;
    if (!read_normalized(full, content))
        return {false, "Error: cannot open '" + full + "'"};

    // Normalise the search text the same way the file was normalised
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
// replace_block  (replace everything between two text anchors, inclusive)
// ---------------------------------------------------------------------------

ToolResult Tools::replace_block(const std::string& path, const std::string& start_anchor,
                                const std::string& end_anchor, const std::string& new_text) {
    auto full = resolve(path);

    if (start_anchor.empty() || end_anchor.empty())
        return {false, "Error: both start_anchor and end_anchor are required."};

    std::string content;
    if (!read_normalized(full, content))
        return {false, "Error: cannot open '" + full + "'"};

    // Split into lines (LF already normalised)
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

    // Capture removed block so the model can verify what was cut
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
// finish_task  (signals autonomous loop completion)
// ---------------------------------------------------------------------------

ToolResult Tools::finish_task(const std::string& summary) {
    task_done_    = true;
    task_summary_ = summary;
    return {true, "Task marked complete."};
}

// ---------------------------------------------------------------------------
// Dispatch
// ---------------------------------------------------------------------------

ToolResult Tools::dispatch(const std::string& name, const nlohmann::json& args) {
    auto str = [&](const char* key, const char* def = "") -> std::string {
        return args.contains(key) ? args[key].get<std::string>() : def;
    };
    auto num = [&](const char* key, int def = 0) -> int {
        if (!args.contains(key)) return def;
        auto& v = args[key];
        return v.is_number() ? v.get<int>() : std::stoi(v.get<std::string>());
    };

    if (name == "list_files")
        return list_files(str("directory"), str("pattern"));
    if (name == "read_file")
        return read_file(str("path"));
    if (name == "read_file_range")
        return read_file_range(str("path"), num("start_line", 1), num("end_line", 0));
    if (name == "write_file")
        return write_file(str("path"), str("content"));
    if (name == "run_command")
        return run_command(str("cmd"));
    if (name == "search_files")
        return search_files(str("pattern"), str("directory"), str("file_glob"));
    if (name == "edit_file")
        return edit_file(str("path"), str("old_text"), str("new_text"));
    if (name == "replace_block")
        return replace_block(str("path"), str("start_anchor"), str("end_anchor"), str("new_text"));
    if (name == "finish_task")
        return finish_task(str("summary"));

    return {false, "Unknown tool: " + name};
}
