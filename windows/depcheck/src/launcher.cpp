// launcher.cpp — depcheck.exe: launch a process tree with depcheck.dll
// grafted in, wait for it, and merge the per-process traces into a report.
//
//   depcheck.exe [reports-dir] -- <command line>
//
// The report leads with the chronological list of every exec (CreateProcess)
// call observed across the whole tree, then the process tree, runtime DLL
// loads, file-open summary, and a dumpbin static-vs-runtime cross-check.
#include <windows.h>
#include <detours.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <algorithm>

// ---------------- trace model ----------------

struct ExecCall {
    ULONGLONG ts;
    DWORD pid;        // caller
    DWORD child;      // new process (0 = the call failed)
    std::string app;  // application name / first token
    std::string cmd;  // full command line
    int injected;     // 1 = relaunched through Detours
};

struct ProcInfo {
    DWORD pid = 0, parent = 0;
    ULONGLONG first_ts = 0;
    std::string exe, cmd;
    std::vector<std::string> loads;      // LoadLibraryExW names, in order
    std::map<std::string, int> opens;    // CreateFileW path -> count
};

struct Model {
    std::vector<ExecCall> execs;
    std::map<DWORD, ProcInfo> procs;
    long long dropped = 0;
};

// ---------------- tiny JSONL field getters ----------------

static bool jnum(const std::string& line, const char* key, long long* out)
{
    std::string needle = "\"" + std::string(key) + "\":";
    size_t p = line.find(needle);
    if (p == std::string::npos)
        return false;
    p += needle.size();
    while (p < line.size() && (line[p] == ' ')) p++;
    *out = strtoll(line.c_str() + p, NULL, 10);
    return true;
}

static bool jstr(const std::string& line, const char* key, std::string* out)
{
    std::string needle = "\"" + std::string(key) + "\":";
    size_t p = line.find(needle);
    if (p == std::string::npos)
        return false;
    p += needle.size();
    while (p < line.size() && (line[p] == ' ')) p++;
    if (p >= line.size() || line[p] != '"')
        return false;
    p++;
    out->clear();
    bool esc = false;
    for (; p < line.size(); p++) {
        char c = line[p];
        if (esc) {
            switch (c) {
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {  // \uXXXX — paths here are ASCII; approximate
                        char32_t cp = 0;
                        for (int k = 1; k <= 4 && p + k < line.size(); k++) {
                            char h = line[p + k];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= h - '0';
                            else if (h >= 'a' && h <= 'f') cp |= h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') cp |= h - 'A' + 10;
                        }
                        p += 4;
                        out->push_back(cp < 128 ? (char)cp : '?');
                        break; }
            default: out->push_back(c); break;   // \" \\ \/ etc.
            }
            esc = false;
        } else if (c == '\\') {
            esc = true;
        } else if (c == '"') {
            return true;
        } else {
            out->push_back(c);
        }
    }
    return false;
}

// ---------------- trace ingestion ----------------

static std::string read_file(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE)
        return "";
    std::string data;
    char buf[8192];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof(buf), &got, NULL) && got > 0)
        data.append(buf, got);
    CloseHandle(f);
    return data;
}

static void ingest_trace(Model& m, const std::string& data)
{
    size_t pos = 0;
    while (pos < data.size()) {
        size_t eol = data.find('\n', pos);
        if (eol == std::string::npos) eol = data.size();
        std::string line = data.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.size() < 10)
            continue;

        long long ts = 0, pid = 0;
        jnum(line, "ts", &ts);
        jnum(line, "pid", &pid);
        std::string ev;
        if (!jstr(line, "ev", &ev))
            continue;

        ProcInfo& p = m.procs[(DWORD)pid];
        p.pid = (DWORD)pid;
        if (p.first_ts == 0) p.first_ts = (ULONGLONG)ts;

        if (ev == "attach") {
            jstr(line, "exe", &p.exe);
        } else if (ev == "exec") {
            ExecCall e;
            e.ts = (ULONGLONG)ts;
            e.pid = (DWORD)pid;
            long long child = 0, inj = 0;
            jnum(line, "child", &child);
            jnum(line, "inj", &inj);
            e.child = (DWORD)child;
            e.injected = (int)inj;
            jstr(line, "app", &e.app);
            jstr(line, "cmd", &e.cmd);
            m.execs.push_back(e);

            ProcInfo& c = m.procs[e.child];
            c.pid = e.child;
            c.parent = (DWORD)pid;
            if (c.first_ts == 0) c.first_ts = e.ts;
            if (c.exe.empty()) c.exe = e.app;
            if (c.cmd.empty()) c.cmd = e.cmd;
        } else if (ev == "open") {
            std::string path;
            jstr(line, "path", &path);
            p.opens[path]++;
        } else if (ev == "load") {
            std::string name;
            jstr(line, "name", &name);
            p.loads.push_back(name);
        } else if (ev == "meta") {
            long long dropped = 0;
            jnum(line, "dropped", &dropped);
            m.dropped += dropped;
        }
    }
}

// ---------------- helpers ----------------

static std::string basename_of(const std::string& path)
{
    size_t s = path.find_last_of("/\\");
    std::string b = (s == std::string::npos) ? path : path.substr(s + 1);
    size_t dot = b.find_last_of('.');
    std::string ext = (dot == std::string::npos) ? "" : b.substr(dot + 1);
    for (auto& c : ext) c = (char)tolower(c);
    return b;
}

static std::string ext_of(const std::string& path)
{
    size_t s = path.find_last_of("/\\");
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || (s != std::string::npos && dot < s))
        return "(none)";
    std::string e = path.substr(dot + 1);
    if (e.size() > 8) return "(long)";
    for (auto& c : e) c = (char)tolower(c);
    return e;
}

static std::string proc_name(const Model& m, DWORD pid)
{
    auto it = m.procs.find(pid);
    if (it == m.procs.end() || it->second.exe.empty())
        return "?" + std::to_string(pid);
    return basename_of(it->second.exe);
}

static std::string trunc(const std::string& s, size_t n)
{
    if (s.size() <= n) return s;
    return s.substr(0, n - 3) + "...";
}

static void md_escape(std::string& s)
{
    std::string o;
    for (char c : s) {
        if (c == '|') o += "\\|";
        else if (c == '\n' || c == '\r') o += ' ';
        else o += c;
    }
    s = o;
}

// ---------------- dumpbin cross-check ----------------

static bool run_dumpbin(const std::string& exe, std::vector<std::string>* deps,
                        std::vector<std::string>* delay)
{
    std::wstring wexe(exe.begin(), exe.end());
    std::wstring wcmd = L"dumpbin /dependents \"" + wexe + L"\" 2>&1";
    FILE* pipe = _wpopen(wcmd.c_str(), L"rt");
    if (!pipe)
        return false;

    std::vector<std::string>* cur = nullptr;
    char line[1024];
    bool usable = true;
    while (fgets(line, sizeof(line), pipe)) {
        std::string l(line);
        if (l.find("is not recognized") != std::string::npos
            || l.find("not an internal or external") != std::string::npos) {
            usable = false;
            break;
        }
        if (l.find("following dependencies:") != std::string::npos) {
            cur = (l.find("delay load") != std::string::npos) ? delay : deps;
            continue;
        }
        if (l.find("Summary") != std::string::npos) {
            cur = nullptr;
            continue;
        }
        if (cur) {
            std::string t = l;
            size_t b = t.find_first_not_of(" \t\r\n");
            size_t e = t.find_last_not_of(" \t\r\n");
            if (b == std::string::npos)
                continue;
            t = t.substr(b, e - b + 1);
            // only DLL names; skip headers, notes and section dividers
            if (t.size() > 4 && t.size() < 64) {
                std::string low = t;
                for (auto& c : low) c = (char)tolower(c);
                if (low.substr(low.size() - 4) == ".dll")
                    cur->push_back(t);
            }
        }
    }
    _pclose(pipe);
    return usable;
}

// ---------------- report generation ----------------

static void write_tree_line(FILE* md, const Model& m, DWORD pid, int depth,
                            std::set<DWORD>& visited)
{
    if (!visited.insert(pid).second)
        return;
    const ProcInfo& p = m.procs.at(pid);
    std::string name = basename_of(p.exe.empty() ? "?" : p.exe);
    fprintf(md, "%*s- pid %lu %s", depth * 2, "", (unsigned long)pid, name.c_str());
    if (!p.cmd.empty()) {
        std::string c = trunc(p.cmd, 100);
        md_escape(c);
        fprintf(md, " — `%s`", c.c_str());
    }
    fprintf(md, "\n");
    std::vector<DWORD> kids;
    for (auto& kv : m.procs)
        if (kv.second.parent == pid && kv.first != pid)
            kids.push_back(kv.first);
    for (DWORD k : kids)
        write_tree_line(md, m, k, depth + 1, visited);
}

static void generate_report(const wchar_t* reports_dir, const std::wstring& target,
                            DWORD exit_code, Model& m)
{
    std::wstring rmd = std::wstring(reports_dir) + L"\\report.md";
    std::wstring rjson = std::wstring(reports_dir) + L"\\report.json";
    FILE* md = _wfopen(rmd.c_str(), L"wb");
    FILE* js = _wfopen(rjson.c_str(), L"wb");
    if (!md || !js) {
        if (md) fclose(md);
        if (js) fclose(js);
        return;
    }

    ULONGLONG t0 = m.execs.empty() ? 0 : m.execs[0].ts;
    int injected_count = 0, failed = 0;
    for (auto& e : m.execs) {
        injected_count += e.injected;
        if (e.child == 0) failed++;
    }

    std::string tgt(target.begin(), target.end());

    fprintf(md, "# depcheck report\n\n");
    fprintf(md, "- target: `%s`\n", tgt.c_str());
    fprintf(md, "- exit code: %lu\n", (unsigned long)exit_code);
    fprintf(md, "- processes traced: %zu\n", m.procs.size());
    fprintf(md, "- exec calls observed: %zu (%d through Detours, %d failed)\n",
            m.execs.size(), injected_count, failed);
    if (m.dropped)
        fprintf(md, "- WARNING: %lld events dropped (ring full)\n", m.dropped);

    // ---- the exec list ----
    fprintf(md, "\n## Exec calls (all process creations, chronological)\n\n");
    fprintf(md, "| # | t (ms) | caller | pid | program | inj |\n");
    fprintf(md, "|---|--------|--------|-----|---------|-----|\n");
    int i = 1;
    for (auto& e : m.execs) {
        std::string app = trunc(e.app.empty() ? e.cmd : e.app, 70);
        md_escape(app);
        fprintf(md, "| %d | +%llu | %s (%lu) | %lu | `%s` | %s |\n", i++,
                (unsigned long long)(e.ts - t0),
                proc_name(m, e.pid).c_str(), (unsigned long)e.pid,
                (unsigned long)e.child, app.c_str(),
                e.child ? (e.injected ? "y" : "-") : "FAIL");
    }

    fprintf(md, "\n### Full command lines\n\n");
    i = 1;
    for (auto& e : m.execs) {
        std::string c = e.cmd.empty() ? e.app : e.cmd;
        fprintf(md, "%d. `", i++);
        for (char ch : c)
            fputc(ch == '\n' || ch == '\r' ? ' ' : ch, md);
        fprintf(md, "`\n");
    }

    // ---- process tree ----
    fprintf(md, "\n## Process tree\n\n```\n");
    {
        std::set<DWORD> visited;
        for (auto& kv : m.procs)
            if (kv.second.parent == 0 || !m.procs.count(kv.second.parent))
                write_tree_line(md, m, kv.first, 0, visited);
    }
    fprintf(md, "```\n");

    // ---- DLL loads ----
    fprintf(md, "\n## Runtime DLL loads (LoadLibraryExW observed)\n\n");
    for (auto& kv : m.procs) {
        const ProcInfo& p = kv.second;
        if (p.loads.empty())
            continue;
        fprintf(md, "### pid %lu — %s\n\n", (unsigned long)kv.first,
                basename_of(p.exe.empty() ? "?" : p.exe).c_str());
        std::set<std::string> seen;
        int n = 1;
        for (auto& l : p.loads) {
            if (seen.insert(basename_of(l)).second) {
                fprintf(md, "%d. `%s`\n", n++, l.c_str());
            }
        }
        fprintf(md, "\n");
    }

    // ---- file opens ----
    fprintf(md, "\n## File opens by extension\n\n");
    {
        std::map<std::string, int> hist;
        for (auto& kv : m.procs)
            for (auto& f : kv.second.opens)
                hist[ext_of(f.first)] += f.second;
        fprintf(md, "| extension | opens |\n|---|---|\n");
        for (auto& h : hist)
            fprintf(md, "| %s | %d |\n", h.first.c_str(), h.second);
    }

    // ---- static vs runtime ----
    fprintf(md, "\n## Static imports vs runtime loads (dumpbin cross-check)\n\n");
    {
        std::set<std::string> checked;
        bool any = false;
        for (auto& e : m.execs) {
            if (!e.child || e.app.empty())
                continue;
            std::string base = basename_of(e.app);
            std::string low = base;
            for (auto& c : low) c = (char)tolower(c);
            if (low.size() < 5 || low.substr(low.size() - 4) != ".exe")
                continue;
            if (!checked.insert(low).second)
                continue;
            std::vector<std::string> deps, delay;
            if (!run_dumpbin(e.app, &deps, &delay))
                continue;
            any = true;
            fprintf(md, "### %s\n\n", base.c_str());
            fprintf(md, "- static imports: ");
            for (size_t k = 0; k < deps.size(); k++)
                fprintf(md, "%s`%s`", k ? ", " : "", basename_of(deps[k]).c_str());
            fprintf(md, "\n- delay imports: ");
            for (size_t k = 0; k < delay.size(); k++)
                fprintf(md, "%s`%s`", k ? ", " : "", basename_of(delay[k]).c_str());
            fprintf(md, "\n");

            // observed loads for the process running this exe
            std::set<std::string> observed;
            for (auto& kv : m.procs) {
                if (basename_of(kv.second.exe) != base)
                    continue;
                for (auto& l : kv.second.loads) {
                    std::string b = basename_of(l);
                    for (auto& c : b) c = (char)tolower(c);
                    observed.insert(b);
                }
            }
            std::set<std::string> stat(deps.begin(), deps.end());
            std::set<std::string> dly(delay.begin(), delay.end());
            fprintf(md, "- loaded at runtime but NOT in the static import table: ");
            bool first = true;
            for (auto& o : observed) {
                if (!stat.count(o) && !dly.count(o)) {
                    fprintf(md, "%s`%s`", first ? "" : ", ", o.c_str());
                    first = false;
                }
            }
            if (first) fprintf(md, "(none)");
            fprintf(md, "\n\n");
        }
        if (!any)
            fprintf(md, "(dumpbin unavailable or no .exe targets)\n\n");
    }

    fclose(md);

    // ---- minimal JSON mirror ----
    fprintf(js, "{\n  \"target\": \"%s\",\n  \"exit_code\": %lu,\n  \"execs\": [\n",
            tgt.c_str(), (unsigned long)exit_code);
    for (size_t k = 0; k < m.execs.size(); k++) {
        ExecCall& e = m.execs[k];
        std::string c = e.cmd.empty() ? e.app : e.cmd;
        for (auto& ch : c) if (ch == '\\' || ch == '"') ch = ' ';
        fprintf(js, "    {\"t\": %llu, \"caller\": \"%s\", \"pid\": %lu, "
                "\"child\": %lu, \"cmd\": \"%s\", \"injected\": %d}%s\n",
                (unsigned long long)(e.ts - t0), proc_name(m, e.pid).c_str(),
                (unsigned long)e.pid, (unsigned long)e.child, c.c_str(),
                e.injected, k + 1 < m.execs.size() ? "," : "");
    }
    fprintf(js, "  ]\n}\n");
    fclose(js);
}

// ---------------- main ----------------

static void ensure_dir(const std::wstring& dir)
{
    // create each path component (reports, reports\build, ...)
    std::wstring cur;
    for (size_t i = 0; i < dir.size(); i++) {
        if (dir[i] == L'\\' && cur.size() > 2) {
            CreateDirectoryW(cur.c_str(), NULL);
        }
        cur.push_back(dir[i]);
    }
    CreateDirectoryW(dir.c_str(), NULL);
}

static void clean_traces(const std::wstring& dir)
{
    std::wstring pattern = dir + L"\\trace-*.jsonl";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

int wmain(int argc, wchar_t** argv)
{
    // NB: do NOT SetConsoleOutputCP here — the traced cmd.exe inherits this
    // console, and cmd's batch parser misreads scripts under a UTF-8
    // codepage (lines execute with a byte offset).

    int sep = -1;
    for (int i = 1; i < argc; i++)
        if (wcscmp(argv[i], L"--") == 0) { sep = i; break; }
    if (sep < 0 || sep + 1 >= argc) {
        printf("usage: depcheck [reports-dir] -- <program> [args...]\n");
        return 2;
    }

    // reports dir: explicit arg before --, else .\reports
    const wchar_t* dirarg = (sep >= 2) ? argv[1] : L"reports";
    wchar_t reports[MAX_PATH * 2];
    if (!GetFullPathNameW(dirarg, MAX_PATH * 2, reports, NULL)) {
        printf("depcheck: bad reports dir\n");
        return 2;
    }
    ensure_dir(reports);
    clean_traces(reports);
    SetEnvironmentVariableW(L"DEPCHECK_REPORTS", reports);

    // payload DLL lives next to this exe
    wchar_t dll[MAX_PATH * 2];
    GetModuleFileNameW(NULL, dll, MAX_PATH * 2);
    wchar_t* slash = wcsrchr(dll, L'\\');
    if (!slash) {
        printf("depcheck: cannot locate payload dll\n");
        return 2;
    }
    wcscpy(slash + 1, L"depcheck.dll");

    // build the target command line (quote args containing spaces)
    std::vector<wchar_t> cmdline;
    for (int i = sep + 1; i < argc; i++) {
        if (i > sep + 1) cmdline.push_back(L' ');
        bool q = wcspbrk(argv[i], L" \t") != NULL || argv[i][0] == 0;
        if (q) cmdline.push_back(L'"');
        for (const wchar_t* p = argv[i]; *p; p++) cmdline.push_back(*p);
        if (q) cmdline.push_back(L'"');
    }
    cmdline.push_back(L'\0');

    // Script targets (.cmd/.bat) must be run through an explicit
    // `cmd.exe /s /c "..."`. Handing the script straight to CreateProcessW
    // lets the kernel's implicit batch wrapping corrupt cmd's parsing of
    // the script once the Detours graft is in play.
    const wchar_t* first = argv[sep + 1];
    const wchar_t* dot = wcsrchr(first, L'.');
    bool is_script = dot
        && (_wcsicmp(dot, L".cmd") == 0 || _wcsicmp(dot, L".bat") == 0);
    std::vector<wchar_t> final_cmd;
    if (is_script) {
        const wchar_t prefix[] = L"cmd.exe /s /c \"";
        final_cmd.assign(prefix, prefix + wcslen(prefix));
        final_cmd.insert(final_cmd.end(), cmdline.begin(), cmdline.end() - 1);
        final_cmd.push_back(L'"');
        final_cmd.push_back(L'\0');
    } else {
        final_cmd = cmdline;
    }

    printf("depcheck: reports -> %ls\n", reports);
    printf("depcheck: payload  -> %ls\n", dll);
    printf("depcheck: target   -> %ls\n", final_cmd.data());

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    // Forward our own startup info (std handles + flags) and let the child
    // inherit handles — the canonical Detours-launcher setup (withdll /
    // tracebld). A zeroed STARTUPINFO breaks cmd.exe's batch parsing once
    // the Detours graft is active.
    GetStartupInfoW(&si);

    // DetourCreateProcessWithDllW takes the DLL name as LPCSTR (Detours quirk)
    char adll[MAX_PATH * 2];
    WideCharToMultiByte(CP_ACP, 0, dll, -1, adll, sizeof(adll), NULL, NULL);

    if (!DetourCreateProcessWithDllW(NULL, final_cmd.data(), NULL, NULL, TRUE, 0,
                                     NULL, NULL, &si, &pi, adll, nullptr)) {
        printf("depcheck: DetourCreateProcessWithDllW failed: %lu\n",
               GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);

    // give lingering writers a moment, then merge everything we can read
    Sleep(300);

    Model m;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((std::wstring(reports) + L"\\trace-*.jsonl").c_str(),
                              &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            ingest_trace(m, read_file((std::wstring(reports) + L"\\" + fd.cFileName).c_str()));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // traces merge in directory order; every chronology must be global
    std::stable_sort(m.execs.begin(), m.execs.end(),
                     [](const ExecCall& a, const ExecCall& b) { return a.ts < b.ts; });

    generate_report(reports, final_cmd.data(), code, m);
    printf("\ndepcheck: target exited %lu; %zu exec calls, %zu processes traced\n",
           (unsigned long)code, m.execs.size(), m.procs.size());
    printf("depcheck: report -> %ls\\report.md\n", reports);
    return 0;
}
