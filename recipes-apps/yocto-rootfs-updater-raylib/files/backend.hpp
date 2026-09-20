// backend.hpp
//
// Backend logic for the Yocto RootFS Updater. No libcurl/OpenSSL/JSON
// dependency: all actions go through existing system tools (lsblk,
// mount, umount, tar, curl, sha256sum). Only library dependencies are
// libc, raylib and raygui.
#pragma once

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <functional>
#include <map>
#include <optional>
#include <signal.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace backend {

struct OperationError : public std::runtime_error {
    explicit OperationError(const std::string& msg) : std::runtime_error(msg) {}
};

using LogFn = std::function<void(const std::string&)>;
using ProgressFn = std::function<void(long long done, long long total, const std::string& status)>;

// ---------------------------------------------------------------------
// Process execution via fork/exec - no shell interpolation, so no
// quoting issues with paths/URLs containing spaces.
// ---------------------------------------------------------------------

struct ProcResult {
    int exit_code = -1;
    std::string output;
};

inline std::vector<char*> to_argv(const std::vector<std::string>& args) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);
    return argv;
}

inline ProcResult exec_capture(const std::vector<std::string>& args) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {-1, "pipe() failed"};
    pid_t pid = fork();
    if (pid < 0) return {-1, "fork() failed"};
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        auto argv = to_argv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) out.append(buf, (size_t)n);
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    ProcResult r;
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    r.output = out;
    return r;
}

// Non-blocking, stdout/stderr to /dev/null (for downloads, whose
// progress we track via the destination file size instead).
inline pid_t exec_async_silent(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        auto argv = to_argv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    return pid;
}

// Blocking, reads output line by line and calls a callback per line
// (e.g. for "tar -v" progress). Can be cancelled via cancel_flag
// (sends SIGTERM to the child).
inline int exec_stream_lines(const std::vector<std::string>& args,
                              const std::function<void(const std::string&)>& on_line,
                              std::atomic<bool>* cancel_flag) {
    int pipefd[2];
    if (pipe(pipefd) != 0) throw OperationError("pipe() failed");
    pid_t pid = fork();
    if (pid < 0) throw OperationError("fork() failed");
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        auto argv = to_argv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    std::string linebuf;
    char buf[4096];
    bool eof = false;
    while (!eof) {
        if (cancel_flag && cancel_flag->load()) {
            kill(pid, SIGTERM);
        }
        ssize_t n = read(pipefd[0], buf, sizeof(buf));
        if (n > 0) {
            linebuf.append(buf, (size_t)n);
            size_t pos;
            while ((pos = linebuf.find('\n')) != std::string::npos) {
                on_line(linebuf.substr(0, pos));
                linebuf.erase(0, pos + 1);
            }
        } else if (n == 0) {
            eof = true;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(20000);
            } else {
                eof = true;
            }
        }
    }
    close(pipefd[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

inline std::string join(const std::vector<std::string>& v, const char* sep = " ") {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += sep;
        s += v[i];
    }
    return s;
}

// Blocking, stdout/stderr redirected directly to /dev/null at the OS
// level (never captured into this process's own memory at all, not
// just "captured but then discarded") - for commands whose ARGUMENTS
// or OUTPUT could contain a secret (a WiFi passphrase, currently the
// only such case in this codebase) that must never appear anywhere
// this app itself could expose it (the on-screen log, and by
// extension the systemd journal if that log's own content were ever
// written there or exported). Found via a real, direct security
// report: run_checked()'s own unconditional "log(\"$ \" + join(args))"
// was putting a WiFi passphrase, passed as a plain --passphrase
// command-line argument, directly into the visible, on-screen log.
// Callers of this function must log their OWN, already-redacted
// version of the command themselves (see connect_wifi_network()) -
// this function deliberately does no logging of the command or its
// output at all, so there's nothing here for a caller to accidentally
// get wrong by passing the real, unredacted args through to a shared
// logging path.
inline void run_checked_silent(const std::vector<std::string>& args) {
    pid_t pid = fork();
    if (pid < 0) throw OperationError("fork() failed");
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        auto argv = to_argv(args);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code != 0) {
        // Deliberately does NOT include join(args) here either (that
        // would put the real, unredacted command - passphrase
        // included - into an exception message that could itself end
        // up logged/displayed) - just the bare exit code.
        throw OperationError("Command failed, exit code " + std::to_string(exit_code));
    }
}

inline void run_checked(const std::vector<std::string>& args, const LogFn& log) {
    log("$ " + join(args));
    ProcResult r = exec_capture(args);
    if (!r.output.empty()) log(r.output);
    if (r.exit_code != 0) {
        throw OperationError("Command failed (" + join(args) + "), exit code " +
                              std::to_string(r.exit_code));
    }
}

inline std::string human_size(double n) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    int u = 0;
    while (n >= 1024.0 && u < 4) { n /= 1024.0; ++u; }
    char buf[64];
    snprintf(buf, sizeof(buf), "%.1f%s", n, units[u]);
    return buf;
}

// A path is a mountpoint if its st_dev differs from its parent's.
inline bool is_mountpoint(const std::string& path) {
    struct stat st_path{}, st_parent{};
    if (stat(path.c_str(), &st_path) != 0) return false;
    std::string parent = path;
    while (parent.size() > 1 && parent.back() == '/') parent.pop_back();
    size_t slash = parent.find_last_of('/');
    parent = (slash == std::string::npos) ? "/" : (slash == 0 ? "/" : parent.substr(0, slash));
    if (stat(parent.c_str(), &st_parent) != 0) return false;
    return st_path.st_dev != st_parent.st_dev;
}

// access(path, W_OK) alone only checks permission BITS - a directory
// can have full 0777 permissions and still be entirely unwritable if
// the whole FILESYSTEM underneath it is mounted read-only (exactly
// initramfs-home-mount's own case: /mnt/storage deliberately mounted
// "-o ro" there, on request, this project's own rescue-context "never
// write back to source media" stance - see that recipe's own files/
// initramfs-home-mount comments). statvfs()'s own f_flag/ST_RDONLY
// bit is the correct, standard way to check this at the filesystem
// level directly, independent of permission bits entirely. Found via
// a real user report: an HTTP(S) rootfs download failed with curl
// exit code 23 ("failed writing received data") once /mnt/storage
// started actually being mounted (readonly) via initramfs-home-mount
// - main.cpp's own download_staging_dir() had been preferring /mnt/
// storage over /tmp whenever it was mounted AT ALL, written back when
// that mount was only ever writable (via storage-partition-helper on
// the disk-backed image) in practice up to this point.
inline bool is_writable_mountpoint(const std::string& path) {
    struct statvfs vfs{};
    if (statvfs(path.c_str(), &vfs) != 0) return false;
    return !(vfs.f_flag & ST_RDONLY);
}

inline bool is_mounted_at(const std::string& mountpoint) {
    return is_mountpoint(mountpoint);
}

// ---------------------------------------------------------------------
// Partitions, via "lsblk -P" (KEY="VALUE" pairs - easy to parse
// without a JSON library).
// ---------------------------------------------------------------------

struct PartitionInfo {
    std::string name, path, size, type, fstype, mountpoint, label, uuid, pkname;
};

// Finds which physical disk (PKNAME) a given file path actually lives
// on, by longest-matching mountpoint among all currently mounted
// partitions - same principle the OS itself uses to resolve "which
// filesystem does this path belong to". Returns empty string if no
// match is found (e.g. path doesn't exist, or somehow isn't under any
// known mountpoint).
inline std::string disk_containing_path(const std::string& file_path,
                                         const std::vector<PartitionInfo>& partitions) {
    std::string best_mountpoint, best_pkname;
    for (auto& p : partitions) {
        if (p.mountpoint.empty()) continue;
        // Longest-prefix match, with a trailing '/' boundary check so
        // e.g. "/mnt/storage2" doesn't wrongly match a file actually
        // under "/mnt/storage".
        if (file_path.compare(0, p.mountpoint.size(), p.mountpoint) == 0 &&
            (file_path.size() == p.mountpoint.size() || file_path[p.mountpoint.size()] == '/' ||
             p.mountpoint == "/")) {
            if (p.mountpoint.size() > best_mountpoint.size()) {
                best_mountpoint = p.mountpoint;
                best_pkname = p.pkname;
            }
        }
    }
    return best_pkname;
}

inline std::vector<std::pair<std::string, std::string>> parse_kv_pairs(const std::string& line) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && isspace((unsigned char)line[i])) ++i;
        size_t key_start = i;
        while (i < line.size() && line[i] != '=') ++i;
        if (i >= line.size()) break;
        std::string key = line.substr(key_start, i - key_start);
        ++i;
        if (i >= line.size() || line[i] != '"') break;
        ++i;
        size_t val_start = i;
        while (i < line.size() && line[i] != '"') ++i;
        std::string val = line.substr(val_start, i - val_start);
        ++i;
        out.emplace_back(key, val);
    }
    return out;
}

inline std::vector<PartitionInfo> list_partitions(const LogFn& log) {
    ProcResult r = exec_capture({"lsblk", "-P", "-b", "-o",
                                  "NAME,PATH,SIZE,TYPE,FSTYPE,MOUNTPOINT,LABEL,UUID,PKNAME"});
    if (r.exit_code != 0) {
        log("lsblk error: " + r.output);
        return {};
    }
    std::vector<PartitionInfo> result;
    size_t pos = 0;
    while (pos < r.output.size()) {
        size_t nl = r.output.find('\n', pos);
        std::string line = r.output.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? r.output.size() : nl + 1;
        if (line.empty()) continue;
        PartitionInfo p;
        for (auto& kv : parse_kv_pairs(line)) {
            if (kv.first == "NAME") p.name = kv.second;
            else if (kv.first == "PATH") p.path = kv.second;
            else if (kv.first == "SIZE") p.size = kv.second;
            else if (kv.first == "TYPE") p.type = kv.second;
            else if (kv.first == "FSTYPE") p.fstype = kv.second;
            else if (kv.first == "MOUNTPOINT") p.mountpoint = kv.second;
            else if (kv.first == "LABEL") p.label = kv.second;
            else if (kv.first == "UUID") p.uuid = kv.second;
            else if (kv.first == "PKNAME") p.pkname = kv.second;
        }
        if (p.type == "part" || p.type == "disk") result.push_back(p);
    }
    return result;
}

inline std::string partition_label(const PartitionInfo& p) {
    std::string ident = p.uuid.empty() ? (p.path.empty() ? p.name : p.path) : ("UUID=" + p.uuid);
    double bytes = 0;
    try { bytes = std::stod(p.size); } catch (...) {}
    std::string s = ident + "  [" + p.type + "]  " + human_size(bytes);
    if (!p.fstype.empty()) s += "  " + p.fstype;
    if (!p.label.empty()) s += "  " + p.label;
    if (!p.mountpoint.empty()) s += "  (mounted: " + p.mountpoint + ")";
    return s;
}

inline std::string partition_device_id(const PartitionInfo& p) {
    if (!p.uuid.empty()) return "UUID=" + p.uuid;
    return p.path.empty() ? p.name : p.path;
}

// ---------------------------------------------------------------------
// Mount / unmount
// ---------------------------------------------------------------------

inline void mkdirs(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            if (cur.size() > 1) mkdir(cur.c_str(), 0755);
        }
    }
}

inline void do_mount(const std::string& device, const std::string& mountpoint, const LogFn& log) {
    mkdirs(mountpoint);
    if (is_mounted_at(mountpoint)) {
        throw OperationError(mountpoint + " is already a mountpoint. Please check manually first.");
    }
    run_checked({"mount", "-t", "auto", device, mountpoint}, log);
}

inline void do_unmount(const std::string& mountpoint, const LogFn& log) {
    if (is_mounted_at(mountpoint)) {
        run_checked({"umount", mountpoint}, log);
    }
}

#include <dirent.h>

inline void remove_recursive(const std::string& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (d) {
            struct dirent* entry;
            while ((entry = readdir(d)) != nullptr) {
                std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                remove_recursive(path + "/" + name);
            }
            closedir(d);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

// Clears a mountpoint, but skips anything that is itself a mountpoint
// (e.g. a separately mounted /home) automatically.
inline void clear_directory(const std::string& mountpoint,
                             const std::vector<std::string>& extra_excludes,
                             const LogFn& log) {
    std::vector<std::string> excludes = {"lost+found"};
    for (auto& e : extra_excludes) {
        std::string t = e;
        while (!t.empty() && t.front() == '/') t.erase(t.begin());
        while (!t.empty() && t.back() == '/') t.pop_back();
        if (!t.empty()) excludes.push_back(t);
    }
    DIR* d = opendir(mountpoint.c_str());
    if (!d) throw OperationError("Could not open " + mountpoint + ".");
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        bool excluded = false;
        for (auto& ex : excludes) if (ex == name) { excluded = true; break; }
        std::string full = mountpoint + "/" + name;
        if (excluded) {
            log("  skipping (excluded): " + name);
            continue;
        }
        struct stat st{};
        if (lstat(full.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) &&
            is_mountpoint(full)) {
            log("  skipping (own partition): " + name);
            continue;
        }
        remove_recursive(full);
    }
    closedir(d);
}

inline std::string sha256_of(const std::string& path) {
    ProcResult r = exec_capture({"sha256sum", path});
    if (r.exit_code != 0) throw OperationError("sha256sum failed:\n" + r.output);
    size_t sp = r.output.find(' ');
    return sp == std::string::npos ? r.output : r.output.substr(0, sp);
}

inline long long get_content_length(const std::string& url) {
    ProcResult r = exec_capture({"curl", "-sIL", url});
    if (r.exit_code != 0) return 0;
    long long len = 0;
    size_t pos = 0;
    while (true) {
        size_t nl = r.output.find('\n', pos);
        std::string line = r.output.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        std::string lower = line;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        if (lower.rfind("content-length:", 0) == 0) {
            try { len = std::stoll(line.substr(line.find(':') + 1)); } catch (...) {}
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return len;
}

// If the given URL looks like a directory (ends with '/'), fetches
// it and looks for an HTML directory listing in the specific,
// predictable format Python's http.server / Apache-style autoindex
// produce (<a href="filename">filename</a> entries) - this is what
// this project's own scripts/serve-https.py serves, and covers a lot
// of similarly simple internal build-artifact servers too, but is NOT
// a general HTTP directory-listing standard (none exists) and won't
// work against servers that don't expose one this way (most CDNs/
// generic web servers deliberately don't). If url doesn't end in '/'
// (already points at a specific file), or looks like a directory but
// no listing/matching files can be found, this throws a clear error
// telling the person to fall back to entering the exact file URL
// instead - never silently returns something wrong.
//
// suffix: case-insensitive file extension to look for (e.g. ".wic",
// ".tar.gz"). Among matches, picks the lexicographically LAST
// filename - relies on the same "sortable name = latest" convention
// this project's own local-file auto-select (find_latest_matching_
// file() in main.cpp) benefits from via mtime instead; over HTTP,
// per-file mtime isn't available without N extra round-trips, so
// filename ordering is the practical alternative here.
// Fetches and parses a directory-listing URL (see
// resolve_possible_directory_url()'s own comments for the format this
// supports and why), returning EVERY matching filename found -
// sorted descending (lexicographically "latest first"), not just the
// single best guess. Used by the GUI's own directory-URL picker
// (main.cpp) to let the person choose explicitly rather than have
// this project silently guess - see that mechanism's own comments for
// why silent guessing turned out not to be a great idea for a
// destructive action. resolve_possible_directory_url() below is
// reimplemented in terms of this, kept only as the worker thread's
// own safety-net fallback in case a directory URL ever reaches it
// without having gone through the picker first (e.g. a
// config.toml-supplied http_default_dir the person never touched).
// --- WiFi (iwd/iwctl) ---
//
// Everything below was verified directly against the real, uploaded
// iwd-3.12 source (client/device.c, client/station.c, client/
// display.c) - not assumed or guessed at, given no working iwd/iwctl
// instance was available in the environment this was written in to
// test against interactively. Real hardware testing is still the
// final word on whether these exact byte offsets hold - see this
// project's own README entry on this feature for that caveat.
//
// iwctl unconditionally emits ANSI color escape codes in its table
// output regardless of whether stdout is a TTY (confirmed directly -
// no isatty()/color-disable check exists anywhere in display.c, and
// there's no --no-color-style command-line option either) - stripped
// here before any parsing, rather than working around them inline.
inline std::string strip_ansi_codes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        if (s[i] == '\x1B' && i + 1 < s.size() && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < s.size() && s[j] != 'm') j++;
            i = (j < s.size()) ? j + 1 : j;
        } else {
            out += s[i];
            i++;
        }
    }
    return out;
}

struct WifiNetworkInfo {
    std::string name;
    std::string security;   // "psk", "open", "8021x", "wep (unsupported)", ...
    bool connected = false;
};

// "iwctl device list" - first (and, for this project's own single-
// adapter target hardware, only expected) wireless device name.
// Column layout confirmed directly in client/device.c
// (display_device_inline(): display_table_row(margin, 5, 20,
// device->name, ...) - MARGIN "  " (2 chars) + 20-char left-justified
// name field, same display_table_row()/entry_append() mechanism as
// the network list below, whose exact "%-*s  " per-field format was
// also confirmed directly in client/display.c.
inline std::string find_wifi_device(const LogFn& log) {
    ProcResult r = exec_capture({"iwctl", "device", "list"});
    if (r.exit_code != 0) return "";
    std::string clean = strip_ansi_codes(r.output);

    std::istringstream iss(clean);
    std::string line;
    bool past_header = false;
    while (std::getline(iss, line)) {
        // Header row itself contains "Name" as its own column title -
        // skip everything up to and including the dashed separator
        // line that follows it (display_table_header() always prints
        // one dashed line, then the column-title row, then another
        // dashed line before actual data rows begin).
        if (!past_header) {
            if (line.find("Name") != std::string::npos) past_header = true;
            continue;
        }
        if (line.find("---") != std::string::npos) continue;
        if (line.size() < 22) continue;
        std::string name = line.substr(2, 20);
        // Trim trailing spaces (left-justified field padding).
        size_t end = name.find_last_not_of(' ');
        if (end == std::string::npos) continue;
        name = name.substr(0, end + 1);
        if (!name.empty()) {
            log("WiFi device found: " + name);
            return name;
        }
    }
    return "";
}

// "iwctl station <device> get-networks" - column layout confirmed
// directly in client/station.c (ordered_networks_display(): header
// format "%s%-*s  %-*s  %-*s  %*s" with MARGIN, 2-wide connection
// indicator, 32-wide "Network name", 18-wide "Security", 6-wide
// right-aligned "Signal"; each data row via the same display_table_
// row()/entry_append() "%-*s  " per-field mechanism). Byte offsets
// after stripping ANSI color codes: connection indicator [2:4],
// network name [6:38], security type [40:58] (signal strength
// intentionally not extracted - this app has no use for it beyond
// what iwd's own GetOrderedNetworks-driven sort order, which get-
// networks already reflects, already provides).
//
// WiFi SSIDs are capped at 32 bytes by the 802.11 standard itself -
// exactly this column's own width - so wrapping onto a second
// printed line (entry_append()/next_line() do this for any value
// wider than its column) shouldn't occur for real-world network
// names in practice, though this isn't exhaustively guarded against
// here.
// Shared parsing core for "iwctl station <device> get-networks"
// output - see list_wifi_networks()'s own comment for the verified
// byte-offset derivation. Factored out so the lightweight connection-
// status check below (is_wifi_connected()) can reuse the exact same,
// already-verified parsing without duplicating it.
inline std::vector<WifiNetworkInfo> parse_wifi_networks_output(const std::string& raw_output) {
    std::vector<WifiNetworkInfo> result;
    std::string clean = strip_ansi_codes(raw_output);

    std::istringstream iss(clean);
    std::string line;
    bool past_header = false;
    while (std::getline(iss, line)) {
        if (!past_header) {
            if (line.find("Network name") != std::string::npos) past_header = true;
            continue;
        }
        if (line.find("---") != std::string::npos) continue;
        if (line.find("No networks available") != std::string::npos) break;
        if (line.size() < 58) continue;

        std::string indicator = line.substr(2, 2);
        std::string name = line.substr(6, 32);
        std::string security = line.substr(40, 18);

        auto trim = [](std::string s) {
            size_t end = s.find_last_not_of(' ');
            return (end == std::string::npos) ? std::string() : s.substr(0, end + 1);
        };
        name = trim(name);
        security = trim(security);
        if (name.empty()) continue;

        WifiNetworkInfo info;
        info.name = name;
        info.security = security;
        info.connected = (indicator.find('>') != std::string::npos);
        result.push_back(info);
    }
    return result;
}

inline std::vector<WifiNetworkInfo> list_wifi_networks(const std::string& device, const LogFn& log) {
    log("Scanning for WiFi networks...");
    // Scan is asynchronous in iwd - fire it, give it a few seconds to
    // populate results, then read back whatever's known by then.
    // iwctl's own "get-networks" reads iwd's already-cached results
    // rather than triggering a scan itself (confirmed: cmd_get_
    // networks() calls GetOrderedNetworks directly, no Scan() call in
    // that same function) - the explicit scan step here is what
    // actually populates that cache in the first place, so skipping
    // it would just repeatedly show stale/empty results.
    exec_capture({"iwctl", "station", device, "scan"});
    struct timespec ts = {3, 0};
    nanosleep(&ts, nullptr);

    ProcResult r = exec_capture({"iwctl", "station", device, "get-networks"});
    if (r.exit_code != 0) {
        log("WARNING: could not list WiFi networks (iwctl exit code " + std::to_string(r.exit_code) + ")");
        return {};
    }
    auto result = parse_wifi_networks_output(r.output);
    log("Found " + std::to_string(result.size()) + " network(s).");
    return result;
}

// Lightweight periodic check (see main.cpp's own polling interval
// comment on why "periodic", not every frame) for whether ANY network
// is currently connected - used to color the WiFi button itself, not
// the full network-list screen. Deliberately skips the scan+3-second-
// wait list_wifi_networks() does: "get-networks" alone already reads
// iwd's already-cached results instantly (see that function's own
// comment on this, confirmed directly against the real source) -
// exactly what a frequent, lightweight poll needs, at the cost of
// only reflecting whatever iwd's cache already knows rather than
// forcing a fresh scan each time (fine for this purpose - connecting/
// disconnecting doesn't require a fresh scan to be reflected in
// iwd's own connection state).
inline bool is_wifi_connected(const std::string& device) {
    if (device.empty()) return false;
    ProcResult r = exec_capture({"iwctl", "station", device, "get-networks"});
    if (r.exit_code != 0) return false;
    for (auto& net : parse_wifi_networks_output(r.output)) {
        if (net.connected) return true;
    }
    return false;
}

// "iwctl [--passphrase <pass>] station <device> connect <ssid>" -
// COMMAND_OPTION_PASSPHRASE confirmed directly in client/command.h
// ("passphrase", short option 'P') and its non-interactive handling
// in client/agent.c (request_passphrase_command_option(): returns the
// value given via this option directly, without prompting, whenever
// iwd's own agent callback asks for one during Connect()). Omit
// entirely for an open network - passing an unused --passphrase
// there would be at best ignored, at worst confusing; the caller
// (main.cpp) only supplies one when the selected network's own
// security type isn't "open".
inline void connect_wifi_network(const std::string& device, const std::string& ssid,
                                  const std::string& passphrase, const LogFn& log) {
    log("Connecting to \"" + ssid + "\"...");
    std::vector<std::string> args = {"iwctl"};
    if (!passphrase.empty()) {
        args.push_back("--passphrase");
        args.push_back(passphrase);
    }
    args.push_back("station");
    args.push_back(device);
    args.push_back("connect");
    args.push_back(ssid);
    // SECURITY: never log the real args (would put the passphrase, a
    // plain command-line argument here, directly into the visible
    // on-screen log) - see run_checked_silent()'s own comment for the
    // full story (found via a direct user report). Logs a redacted
    // stand-in for transparency (still shows THAT a connect was
    // attempted and roughly how), executes with the REAL args via
    // run_checked_silent() instead of run_checked() - stdout/stderr
    // go straight to /dev/null at the OS level, never captured into
    // this process's own memory, let alone logged.
    if (!passphrase.empty()) {
        log("$ iwctl --passphrase *** station " + device + " connect " + ssid);
    } else {
        log("$ " + join(args));
    }
    run_checked_silent(args);
    log("Connected to \"" + ssid + "\".");
}

// "iwctl station <device> disconnect" - confirmed directly in
// client/station.c ("disconnect", cmd_disconnect, no arguments beyond
// the device itself).
inline void disconnect_wifi_network(const std::string& device, const LogFn& log) {
    log("Disconnecting WiFi...");
    run_checked({"iwctl", "station", device, "disconnect"}, log);
    log("Disconnected.");
}

inline std::vector<std::string> list_directory_url_matches(const std::string& url, const std::string& suffix,
                                                             const LogFn& log) {
    if (suffix.size() < 2) {
        throw OperationError("list_directory_url_matches() called with too short/empty a suffix - "
                              "would match every file in the listing, refusing rather than guessing wrong.");
    }

    log("Fetching directory listing: " + url);
    ProcResult r = exec_capture({"curl", "-sL", "--fail", "--max-time", "10", url});
    if (r.exit_code != 0) {
        throw OperationError("Could not fetch directory listing from " + url +
                              " - if this server doesn't provide one, enter the exact file URL instead.");
    }

    std::string suffix_lower = suffix;
    for (auto& c : suffix_lower) c = (char)tolower((unsigned char)c);

    std::vector<std::string> matches;
    const std::string needle = "href=\"";
    size_t pos = 0;
    while (true) {
        size_t start = r.output.find(needle, pos);
        if (start == std::string::npos) break;
        start += needle.size();
        size_t end = r.output.find('"', start);
        if (end == std::string::npos) break;
        std::string href = r.output.substr(start, end - start);
        pos = end + 1;

        // Ignore parent-directory links, subdirectories (trailing
        // '/'), and anything that isn't a same-directory relative
        // filename (absolute URLs/paths, "..").
        if (href.empty() || href.back() == '/' || href.find('/') != std::string::npos) continue;

        std::string href_lower = href;
        for (auto& c : href_lower) c = (char)tolower((unsigned char)c);
        if (href_lower.size() < suffix_lower.size()) continue;
        if (href_lower.compare(href_lower.size() - suffix_lower.size(), suffix_lower.size(), suffix_lower) != 0)
            continue;

        matches.push_back(href);
    }

    std::sort(matches.begin(), matches.end(), std::greater<std::string>());
    return matches;
}

inline std::string resolve_possible_directory_url(const std::string& url, const std::string& suffix,
                                                   const LogFn& log) {
    if (url.empty() || url.back() != '/') return url;   // Already a specific file - nothing to resolve.
    auto matches = list_directory_url_matches(url, suffix, log);
    if (matches.empty()) {
        throw OperationError("No " + suffix + " file found in the directory listing at " + url +
                              " - enter the exact file URL instead.");
    }
    log("Resolved to: " + matches.front());
    return url + matches.front();
}

inline void download(const std::string& url, const std::string& dest,
                      const ProgressFn& progress, std::atomic<bool>* cancel_flag) {
    long long total = get_content_length(url);
    unlink(dest.c_str());
    pid_t pid = exec_async_silent({"curl", "-sL", "--fail", "-o", dest, url});
    bool done = false;
    while (!done) {
        if (cancel_flag && cancel_flag->load()) {
            kill(pid, SIGTERM);
        }
        int status = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        struct stat st{};
        long long size = (stat(dest.c_str(), &st) == 0) ? (long long)st.st_size : 0;
        if (progress) progress(size, total, "Downloading: " + human_size((double)size) + " / " +
                                                 human_size((double)total));
        if (r == pid) {
            done = true;
            if (cancel_flag && cancel_flag->load()) throw OperationError("Download cancelled.");
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                throw OperationError("curl failed (exit code " +
                                      std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
            }
        } else {
            usleep(150000);
        }
    }
}

struct TarStats {
    long long entries = 0;
    long long bytes = 0;
};

inline TarStats scan_tar(const std::string& tar_path, const ProgressFn& progress,
                         std::atomic<bool>* cancel_flag) {
    TarStats st;
    std::string tail;
    int rc = exec_stream_lines(
        {"tar", "-tzvf", tar_path},
        [&](const std::string& line) {
            std::istringstream is(line);
            std::string mode, owner, size;
            is >> mode >> owner >> size;
            if (mode.empty() || (mode[0] != '-' && mode[0] != 'd' && mode[0] != 'l' &&
                                 mode[0] != 'h' && mode[0] != 'c' && mode[0] != 'b' && mode[0] != 'p')) {
                tail = line;
                return;
            }
            ++st.entries;
            char* end = nullptr;
            long long n = strtoll(size.c_str(), &end, 10);
            if (end && *end == '\0' && n > 0) st.bytes += (n + 4095) / 4096 * 4096;
            if (progress && st.entries % 500 == 0)
                progress(0, 1, "Scanning tarball: " + std::to_string(st.entries) + " entries, " +
                                   human_size((double)st.bytes));
        },
        cancel_flag);
    if (cancel_flag && cancel_flag->load()) throw OperationError("Installation cancelled.");
    if (rc != 0) throw OperationError("Tarball appears to be corrupt:\n" + tail);
    return st;
}

inline long long filesystem_capacity(const std::string& path) {
    struct statvfs vfs{};
    if (statvfs(path.c_str(), &vfs) != 0) throw OperationError("statvfs failed for " + path);
    return (long long)(vfs.f_blocks - (vfs.f_bfree - vfs.f_bavail)) * (long long)vfs.f_frsize;
}

inline long long filesystem_available(const std::string& path) {
    struct statvfs vfs{};
    if (statvfs(path.c_str(), &vfs) != 0) throw OperationError("statvfs failed for " + path);
    return (long long)vfs.f_bavail * (long long)vfs.f_frsize;
}

inline long long block_device_size(const std::string& device) {
    int fd = open(device.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw OperationError("Cannot open " + device + ": " + strerror(errno));
    unsigned long long bytes = 0;
    int rc = ioctl(fd, BLKGETSIZE64, &bytes);
    close(fd);
    if (rc != 0) throw OperationError("Cannot determine size of " + device + ": " + strerror(errno));
    return (long long)bytes;
}

inline void extract_tar(const std::string& tar_path, const std::string& mountpoint, long long total,
                         const ProgressFn& progress, const LogFn& log,
                         std::atomic<bool>* cancel_flag) {
    if (total <= 0) total = 1;
    log("Extracting " + std::to_string(total) + " entries...");
    long long done = 0;
    int rc = exec_stream_lines(
        {"tar", "--numeric-owner", "-xpvzf", tar_path, "-C", mountpoint},
        [&](const std::string&) {
            ++done;
            if (progress) progress(done, total, "Extracting: " + std::to_string(done) + "/" +
                                                      std::to_string(total) + " entries");
        },
        cancel_flag);
    if (cancel_flag && cancel_flag->load()) throw OperationError("Installation cancelled.");
    if (rc != 0) throw OperationError("tar exited with code " + std::to_string(rc));
}

// Writes a full wic image to a disk (initial install), via dd, with
// progress from /proc/<pid>/io. For HTTPS sources, curl is piped
// directly into dd (no temp file). No bmaptool: needs Python3, which
// this native build doesn't carry.
// Parses dd's own "status=progress" line, e.g.
// "1234567890 bytes (1.2 GB, 1.1 GiB) copied, 5 s, 246 MB/s" - just
// the leading integer (raw byte count) is needed. Returns -1 if the
// line doesn't start with a number (e.g. an empty/partial line).
inline long long parse_dd_progress_line(const std::string& line) {
    size_t pos = 0;
    while (pos < line.size() && isspace((unsigned char)line[pos])) pos++;
    size_t start = pos;
    while (pos < line.size() && isdigit((unsigned char)line[pos])) pos++;
    if (pos == start) return -1;
    return atoll(line.substr(start, pos - start).c_str());
}

inline void write_disk_image(const std::string& source, bool is_url, const std::string& device,
                              const LogFn& log, const ProgressFn& progress,
                              std::atomic<bool>* cancel_flag) {
    long long total = 0;
    if (is_url) {
        total = get_content_length(source);
    } else {
        struct stat st{};
        if (stat(source.c_str(), &st) != 0) throw OperationError("File not found: " + source);
        total = st.st_size;
    }
    long long disk = block_device_size(device);
    if (total > disk)
        throw OperationError("Image does not fit: " + human_size((double)total) + " image, " +
                             human_size((double)disk) + " disk (" + device + ").");
    if (total <= 0) total = 1;

    std::string ofArg = "of=" + device;
    pid_t curlPid = -1, ddPid;

    // dd's stderr (where status=progress writes its periodic lines)
    // is piped back to us instead of discarded to /dev/null - this is
    // the progress source now (see the polling loop below), replacing
    // /proc/<ddPid>/io. Found via real-world report: write_bytes
    // stayed at 0 throughout a real multi-minute install, jumping
    // straight to 100% only when dd finished - /proc/<pid>/io's
    // write_bytes field needs CONFIG_TASK_IO_ACCOUNTING, an option
    // this project's minimal kernel config was never checked for (the
    // same "assumed it's just there" gap found repeatedly elsewhere
    // in this project). dd's own self-reported byte count needs no
    // special kernel accounting feature at all - it's dd counting its
    // own writes.
    int errPipe[2];
    if (pipe(errPipe) != 0) throw OperationError("pipe() failed");
    int flags = fcntl(errPipe[0], F_GETFL, 0);
    fcntl(errPipe[0], F_SETFL, flags | O_NONBLOCK);

    if (is_url) {
        // curl and dd connected directly via pipe(), not "sh -c curl|dd":
        // otherwise the PID we get back would be the shell's, not dd's
        // (matters for waitpid()/kill() below).
        int pipefd[2];
        if (pipe(pipefd) != 0) throw OperationError("pipe() failed");
        log("$ curl -sL --fail '" + source + "' | dd " + ofArg + " bs=4M status=progress conv=fsync");
        curlPid = fork();
        if (curlPid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDERR_FILENO);
            close(pipefd[0]); close(pipefd[1]); close(errPipe[0]); close(errPipe[1]);
            execlp("curl", "curl", "-sL", "--fail", source.c_str(), (char*)nullptr);
            _exit(127);
        }
        ddPid = fork();
        if (ddPid == 0) {
            dup2(pipefd[0], STDIN_FILENO);
            dup2(errPipe[1], STDERR_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
            close(pipefd[0]); close(pipefd[1]); close(errPipe[0]); close(errPipe[1]);
            execlp("dd", "dd", ofArg.c_str(), "bs=4M", "status=progress", "conv=fsync", (char*)nullptr);
            _exit(127);
        }
        close(pipefd[0]); close(pipefd[1]);
    } else {
        std::string ifArg = "if=" + source;
        log("$ dd " + ifArg + " " + ofArg + " bs=4M status=progress conv=fsync");
        ddPid = fork();
        if (ddPid == 0) {
            dup2(errPipe[1], STDERR_FILENO);
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) dup2(devnull, STDOUT_FILENO);
            close(errPipe[0]); close(errPipe[1]);
            execlp("dd", "dd", ifArg.c_str(), ofArg.c_str(), "bs=4M", "status=progress", "conv=fsync",
                   (char*)nullptr);
            _exit(127);
        }
    }
    close(errPipe[1]);
    if (ddPid < 0) throw OperationError("fork() failed");

    bool done = false;
    long long written = 0;
    std::string partial_line;
    while (!done) {
        if (cancel_flag && cancel_flag->load()) {
            kill(ddPid, SIGTERM);
            if (curlPid > 0) kill(curlPid, SIGTERM);
        }
        // Drain whatever dd has written to stderr since last check.
        // dd rewrites the SAME line in place using '\r' (not '\n')
        // for its periodic status=progress updates, with a final '\n'
        // only at the very end - split on either.
        char buf[4096];
        ssize_t n;
        while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) {
            partial_line.append(buf, (size_t)n);
            size_t sep;
            while ((sep = partial_line.find_first_of("\r\n")) != std::string::npos) {
                long long v = parse_dd_progress_line(partial_line.substr(0, sep));
                if (v >= 0) written = v;
                partial_line.erase(0, sep + 1);
            }
        }
        if (progress) {
            progress(written, total, "Writing image: " + human_size((double)written) + " / " +
                                          human_size((double)total));
        }
        int status = 0;
        pid_t r = waitpid(ddPid, &status, WNOHANG);
        if (r == ddPid) {
            done = true;
            if (curlPid > 0) { int s2 = 0; waitpid(curlPid, &s2, 0); }
            close(errPipe[0]);
            if (cancel_flag && cancel_flag->load()) throw OperationError("Installation cancelled.");
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
                throw OperationError("dd failed (exit code " +
                                      std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")");
            }
        } else {
            // 50ms, not the original 300ms: a small/fast write (small
            // test image, fast SSD) could otherwise complete within a
            // single loop iteration - waitpid() would see the process
            // already exited before any intermediate progress was ever
            // rendered, making the bar appear to never move at all.
            // Found via real-world report ("no visible progress bar
            // during a full install").
            usleep(50000);
        }
    }
    if (progress) progress(total, total, "Image written, syncing...");
    run_checked({"sync"}, log);
}

// Only for marker checks in probe_is_rootfs: checks whether a path
// entry exists (even as a symlink) without resolving it. Needed
// because e.g. /etc/os-release is often a symlink to
// /usr/lib/os-release - stat() would resolve it against our OWN root,
// not the mounted target partition, giving wrong results.
inline bool path_entry_exists(const std::string& p) {
    struct stat st{};
    return lstat(p.c_str(), &st) == 0;
}

inline bool file_exists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0;
}

inline bool file_contains_ci(const std::string& path, const std::string& needle_lower) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (auto& c : content) c = (char)tolower((unsigned char)c);
    return content.find(needle_lower) != std::string::npos;
}

// Reliable rootfs detection: mount read-only and look for rootfs
// markers, rather than trusting filesystem type or label alone (those
// can't distinguish /home from an actual rootfs). On mount failure
// (e.g. already in use), conservatively returns false.
inline bool probe_is_rootfs(const std::string& device, const LogFn& log) {
    const std::string mp = "/tmp/.partition-probe";
    mkdirs(mp);
    if (is_mounted_at(mp)) exec_capture({"umount", mp});

    ProcResult r = exec_capture({"mount", "-o", "ro", device, mp});
    if (r.exit_code != 0) {
        log("  " + device + ": not mountable (skipped)");
        return false;
    }

    auto has_rootfs_markers = [&](const std::string& root) {
        return path_entry_exists(root + "/etc/os-release") ||
               path_entry_exists(root + "/sbin/init") ||
               path_entry_exists(root + "/usr/lib/systemd/systemd") ||
               path_entry_exists(root + "/lib/systemd/systemd") ||
               path_entry_exists(root + "/bin/sh");
    };

    // Not just "any Linux rootfs" but specifically an OpenEmbedded/
    // Yocto build: os-release's CPE_NAME is always hardcoded as
    // "cpe:/o:openembedded:..." regardless of DISTRO_NAME/NAME, which
    // reliably filters out other distros (Fedora, Ubuntu, ...).
    auto is_openembedded_rootfs = [&](const std::string& root) {
        if (!has_rootfs_markers(root)) return false;
        return file_contains_ci(root + "/etc/os-release", "openembedded");
    };

    bool looks_like_rootfs = is_openembedded_rootfs(mp);
    if (!looks_like_rootfs && has_rootfs_markers(mp)) {
        log("  " + device + ": rootfs found, but not OpenEmbedded/Yocto (skipped)");
    }

    if (!looks_like_rootfs && exec_capture({"btrfs", "subvolume", "list", mp}).exit_code == 0) {
        exec_capture({"umount", mp});
        if (exec_capture({"mount", "-o", "ro,subvolid=5", device, mp}).exit_code == 0) {
            ProcResult subvols = exec_capture({"btrfs", "subvolume", "list", mp});
            if (subvols.exit_code == 0) {
                std::istringstream iss(subvols.output);
                std::string line;
                while (std::getline(iss, line)) {
                    size_t pathPos = line.rfind(" path ");
                    if (pathPos == std::string::npos) continue;
                    std::string subvolPath = line.substr(pathPos + 6);
                    if (is_openembedded_rootfs(mp + "/" + subvolPath)) {
                        log("  " + device + ": OpenEmbedded rootfs found in btrfs subvolume \"" + subvolPath + "\"");
                        looks_like_rootfs = true;
                        break;
                    }
                }
            }
        }
    }

    exec_capture({"umount", mp});
    return looks_like_rootfs;
}

// Boot partition matching now happens in main.cpp via PKNAME (same
// disk as a verified OpenEmbedded rootfs) instead of inspecting the
// boot partition's own contents - no extra mount needed there.

// Deliberately minimal TOML subset (flat "key = value" pairs only, no
// tables/arrays/multiline strings) - consistent with this project's
// "no heavy dependency" approach elsewhere (e.g. the hand-rolled
// lsblk KV parser above). Enough for simple directory/filename config.
inline std::map<std::string, std::string> read_simple_toml(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream f(path);
    if (!f) return out;
    std::string line;
    while (std::getline(f, line)) {
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        size_t end = line.find_last_not_of(" \t\r\n");
        line = line.substr(start, end - start + 1);
        if (line.empty()) continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        size_t ks = key.find_first_not_of(" \t");
        size_t ke = key.find_last_not_of(" \t");
        if (ks == std::string::npos) continue;
        key = key.substr(ks, ke - ks + 1);
        size_t vs = val.find_first_not_of(" \t");
        if (vs == std::string::npos) { out[key] = ""; continue; }
        size_t ve = val.find_last_not_of(" \t");
        val = val.substr(vs, ve - vs + 1);
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        out[key] = val;
    }
    return out;
}

inline void copy_file(const std::string& src, const std::string& dst) {
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) throw OperationError("Could not read " + src + ".");
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) { fclose(in); throw OperationError("Could not write " + dst + "."); }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, out);
    fclose(in);
    fclose(out);
}

// clear_directory()'s own excludes matching only ever compares
// against TOP-LEVEL directory entry names (a single opendir()/
// readdir() pass over the mountpoint itself, plain string equality -
// confirmed directly in that function's own code) - a nested path
// like "etc/fstab" can never match a top-level entry name like "etc"
// or "home", so it was silently never actually protected at all
// despite being listed in excludes. Found via a direct user question
// after config.toml's own excludes_default example (this project's
// own file) listed exactly such nested paths ("etc/hostname",
// "etc/machine-id", "etc/fstab").
//
// This backs up (before clear_directory() runs) and later restores
// (after extract_tar() runs) any excludes entries containing a '/' -
// generalizing what backup_fstab()/restore_fstab() used to do only
// for fstab specifically, now for any such nested-path exclude entry.
// Restoring AFTER extraction (not just skipping deletion during the
// clear step) matters even when the NEW rootfs tarball ships its own
// file at that same path (the common case, every rootfs has its own
// /etc/fstab) - this explicitly overwrites whatever the new tarball
// placed there with the preserved original content, the same as
// backup_fstab()/restore_fstab() already did.
struct NestedExcludeBackup {
    std::string relative_path; // e.g. "etc/fstab"
    std::string backup_path;   // temp location holding its saved content
};

inline std::vector<NestedExcludeBackup> backup_nested_excludes(
        const std::string& mountpoint, const std::vector<std::string>& excludes, const LogFn& log) {
    std::vector<NestedExcludeBackup> backups;
    int idx = 0;
    for (auto& e : excludes) {
        if (e.find('/') == std::string::npos) continue; // top-level - clear_directory() itself already handles these correctly
        std::string src = mountpoint + "/" + e;
        struct stat st{};
        if (lstat(src.c_str(), &st) != 0) {
            log("  nested exclude \"" + e + "\" not found on target, nothing to back up.");
            continue;
        }
        std::string backup_path = "/tmp/yocto_updater_nested_backup_" + std::to_string(idx++);
        if (S_ISDIR(st.st_mode)) {
            // Shells out to "cp -a" for a recursive, attribute-
            // preserving directory copy rather than reimplementing
            // one - matches this project's own established pattern
            // of using external tools for this kind of "just get it
            // right" operation elsewhere (tar, sha256sum, etc.).
            mkdirs(backup_path);
            exec_capture({"cp", "-a", src + "/.", backup_path});
        } else {
            copy_file(src, backup_path);
        }
        backups.push_back({e, backup_path});
        log("  nested exclude \"" + e + "\" backed up.");
    }
    return backups;
}

inline void restore_nested_excludes(const std::string& mountpoint,
                                     const std::vector<NestedExcludeBackup>& backups, const LogFn& log) {
    for (auto& b : backups) {
        std::string dest = mountpoint + "/" + b.relative_path;
        size_t slash = dest.find_last_of('/');
        if (slash != std::string::npos) mkdirs(dest.substr(0, slash));
        struct stat st{};
        if (lstat(b.backup_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            mkdirs(dest);
            exec_capture({"cp", "-a", b.backup_path + "/.", dest});
            exec_capture({"rm", "-rf", b.backup_path});
        } else {
            copy_file(b.backup_path, dest);
            unlink(b.backup_path.c_str());
        }
        log("  nested exclude \"" + b.relative_path + "\" restored.");
    }
}

} // namespace backend
