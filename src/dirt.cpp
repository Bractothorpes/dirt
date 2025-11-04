#include <algorithm>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <cstdlib>
#include <fstream>
#include <cctype>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#include <conio.h>
#include <process.h>
#include <shellapi.h>
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <limits.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#endif

namespace fs = std::filesystem;

#if defined(_WIN32)
static const char *HELP_LINE =
    "[\x18/\x19] move  [\x1b[D] collapse  [\x1b[C] expand  Enter open  [f] find  [r] refresh  [g] top  [G] bottom  [q] quit";
#else
static const char *HELP_LINE =
    "[↑/↓] move  [←] collapse  [→] expand  Enter open  [f] find  [r] refresh  [g] top  [G] bottom  [q] quit";
#endif

static constexpr std::uintmax_t SIZE_CAP_BYTES = 2 * 1024 * 1024;

static std::vector<std::string> fallback_editors()
{
  std::vector<std::string> v;
  if (const char *nvim = std::getenv("NVIM"))
    v.emplace_back(nvim);
  v.emplace_back("nvim");
  if (const char *ed = std::getenv("EDITOR"))
    v.emplace_back(ed);
  v.emplace_back("vim");
  v.emplace_back("vi");
  v.emplace_back("less");
#if defined(_WIN32)
  v.emplace_back("notepad");
#endif
  return v;
}

#if defined(_WIN32)
static void enableAnsiOnWindows()
{
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE)
    return;
  DWORD mode = 0;
  if (!GetConsoleMode(hOut, &mode))
    return;
  mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
  SetConsoleMode(hOut, mode);
}
#endif

static void cursor_to(int row, int col = 1) { std::cout << "\033[" << row << ";" << col << "H"; }
static void clear_line() { std::cout << "\033[K"; }
static void hide_cursor(bool hide) { std::cout << (hide ? "\033[?25l" : "\033[?25h"); }
static void use_alt_screen(bool on) { std::cout << (on ? "\033[?1049h" : "\033[?1049l"); }
static void clear_screen() { std::cout << "\033[2J"; }

static int terminal_rows()
{
#if defined(_WIN32)
  CONSOLE_SCREEN_BUFFER_INFO info;
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (GetConsoleScreenBufferInfo(hOut, &info))
    return info.srWindow.Bottom - info.srWindow.Top + 1;
  return 24;
#else
  struct winsize ws{};
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    return ws.ws_row;
  return 24;
#endif
}

static std::string read_key()
{
#if defined(_WIN32)
  int ch = _getch();
  if (ch == 0 || ch == 224)
  {
    int ch2 = _getch();
    switch (ch2)
    {
    case 72:
      return "UP";
    case 80:
      return "DOWN";
    case 75:
      return "LEFT";
    case 77:
      return "RIGHT";
    default:
      return "";
    }
  }
  if (ch == 13)
    return "\n";
  if (ch == 27)
    return "\x1b";
  return std::string(1, static_cast<char>(ch));
#else
  termios oldt{}, raw{};
  tcgetattr(STDIN_FILENO, &oldt);
  raw = oldt;
  raw.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
  char c = 0;
  if (read(STDIN_FILENO, &c, 1) <= 0)
  {
    tcsetattr(STDIN_FILENO, TCSADRAIN, &oldt);
    return "";
  }
  if (c == '\x1b')
  {
    char seq[2]{};
    if (read(STDIN_FILENO, &seq[0], 1) == 1 && read(STDIN_FILENO, &seq[1], 1) == 1)
    {
      tcsetattr(STDIN_FILENO, TCSADRAIN, &oldt);
      if (seq[0] == '[' && seq[1] == 'A')
        return "UP";
      if (seq[0] == '[' && seq[1] == 'B')
        return "DOWN";
      if (seq[0] == '[' && seq[1] == 'D')
        return "LEFT";
      if (seq[0] == '[' && seq[1] == 'C')
        return "RIGHT";
      return "";
    }
    else
    {
      tcsetattr(STDIN_FILENO, TCSADRAIN, &oldt);
      return "\x1b";
    }
  }
  tcsetattr(STDIN_FILENO, TCSADRAIN, &oldt);
  if (c == '\r')
    return "\n";
  return std::string(1, c);
#endif
}

static std::string prompt_user(const std::string &label)
{
  int rows = terminal_rows();
  cursor_to(rows, 1);
  clear_line();

#if !defined(_WIN32)
  termios oldt{}, cooked{};
  tcgetattr(STDIN_FILENO, &oldt);
  cooked = oldt;
  cooked.c_lflag |= (ICANON | ECHO);
  tcsetattr(STDIN_FILENO, TCSADRAIN, &cooked);
#endif

  std::cout << "\033[?25h";
  std::cout << label << std::flush;

  std::string s;
  std::getline(std::cin, s);

  std::cout << "\033[?25l";
  std::cout.flush();

#if !defined(_WIN32)
  tcsetattr(STDIN_FILENO, TCSADRAIN, &oldt);
#endif
  return s;
}

struct Node
{
  fs::path path;
  std::string name;
  Node *parent = nullptr;
  bool isDir = false;
  bool expanded = false;
  std::vector<std::unique_ptr<Node>> children;

  explicit Node(fs::path p, Node *par = nullptr) : path(std::move(p)), parent(par)
  {
    isDir = fs::is_directory(path);
    name = path.filename().empty() ? path.string() : path.filename().string();
  }

  void toggle()
  {
    if (!isDir)
      return;
    expanded = !expanded;
    if (expanded && children.empty())
    {
      try
      {
        std::vector<fs::path> entries;
        for (auto const &e : fs::directory_iterator(path))
          entries.push_back(e.path());
        std::sort(entries.begin(), entries.end(),
                  [](const fs::path &a, const fs::path &b)
                  {
                    bool ad = fs::is_directory(a), bd = fs::is_directory(b);
                    if (ad != bd)
                      return ad > bd;
                    return a.filename().string() < b.filename().string();
                  });
        for (auto &e : entries)
          children.emplace_back(std::make_unique<Node>(e, this));
      }
      catch (...)
      {
      }
    }
  }
};

static void collect_visible(Node *root, int depth, std::vector<std::pair<Node *, int>> &out)
{
  out.emplace_back(root, depth);
  if (root->isDir && root->expanded)
    for (auto &c : root->children)
      collect_visible(c.get(), depth + 1, out);
}
static std::vector<std::pair<Node *, int>> visible_nodes(Node *root)
{
  std::vector<std::pair<Node *, int>> v;
  collect_visible(root, 0, v);
  return v;
}

static std::optional<std::string> exe_dir_path()
{
#if defined(_WIN32)
  char buf[MAX_PATH];
  DWORD len = GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (len == 0 || len == MAX_PATH)
    return std::nullopt;
  return fs::path(buf).parent_path().string();
#else
  // Linux: /proc/self/exe, macOS: _NSGetExecutablePath
  fs::path p;
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string tmp(size, '\0');
  if (_NSGetExecutablePath(tmp.data(), &size) != 0)
    return std::nullopt;
  p = fs::path(tmp).lexically_normal();
#else
  char buf[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n < 0)
    return std::nullopt;
  buf[n] = '\0';
  p = fs::path(buf);
#endif
  return p.parent_path().string();
#endif
}

static std::optional<std::string> read_editor_for_ext(const std::string &ext)
{
  try
  {
    auto dir = exe_dir_path();
    if (!dir)
      return std::nullopt;
    fs::path cfg = fs::path(*dir) / ".dirtconfig";
    if (!fs::exists(cfg))
      return std::nullopt;

    std::ifstream f(cfg);
    std::string line, generic;
    std::string ext_lower = ext;
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);

    while (std::getline(f, line))
    {
      line.erase(0, line.find_first_not_of(" \t\r\n"));
      if (!line.empty())
        line.erase(line.find_last_not_of(" \t\r\n") + 1);
      if (line.empty() || line[0] == '#')
        continue;

      if (line.rfind("editor_generic=", 0) == 0)
      {
        generic = line.substr(15);
      }
      else if (!ext_lower.empty() && line.rfind(ext_lower + "=", 0) == 0)
      {
        return line.substr(ext_lower.size() + 1);
      }
    }

    if (!generic.empty())
      return generic;
  }
  catch (...)
  {
  }
  return std::nullopt;
}

static std::optional<std::string> pick_editor(const std::string &ext = "")
{
  if (auto cfg = read_editor_for_ext(ext))
    return cfg;

  for (auto &e : fallback_editors())
  {
    if (e.empty())
      continue;
#if defined(_WIN32)
    if (fs::path(e).is_absolute())
    {
      if (fs::exists(e))
        return e;
    }
    else
    {
      return e;
    }
#else
    if (!fs::path(e).is_absolute())
      return e;
    if (fs::exists(e))
      return e;
#endif
  }
  return std::nullopt;
}

struct ScopedAltScreenPause
{
  ScopedAltScreenPause()
  {
    std::cout << "\033[?1049l\033[?25h";
    std::cout.flush();
  }
  ~ScopedAltScreenPause()
  {
    std::cout << "\033[?1049h\033[?25l\033[2J\033[H";
    std::cout.flush();
  }
};

static void open_in_editor_at(const fs::path &p, int line)
{
  std::string ext = p.has_extension() ? p.extension().string() : "";
  auto ed = pick_editor(ext);
  if (!ed)
  {
#if defined(_WIN32)
    ShellExecuteA(NULL, "open", p.string().c_str(), NULL, NULL, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + p.string() + "\"";
    std::system(cmd.c_str());
#endif
    return;
  }

  std::string editor = *ed;
  std::string low = editor;
  std::transform(low.begin(), low.end(), low.begin(), ::tolower);
  bool is_vim = (low.find("vim") != std::string::npos);

#if defined(_WIN32)
  if (is_vim && line > 0)
  {
    std::string plus = "+" + std::to_string(line);
    _spawnlp(_P_WAIT, editor.c_str(), editor.c_str(), plus.c_str(), p.string().c_str(), nullptr);
  }
  else
  {
    _spawnlp(_P_WAIT, editor.c_str(), editor.c_str(), p.string().c_str(), nullptr);
  }
#else
  std::string cmd = editor;
  if (is_vim && line > 0)
    cmd += " +" + std::to_string(line);
  cmd += " \"" + p.string() + "\"";
  std::system(cmd.c_str());
#endif
}

static void open_in_editor(const fs::path &p) { open_in_editor_at(p, -1); }

static std::string to_lower_copy(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c)
                 { return std::tolower(c); });
  return s;
}

struct Match
{
  fs::path file;
  int line;
  std::string preview;
};

static std::vector<Match> find_in_files(const fs::path &base, const std::string &query)
{
  std::vector<Match> results;
  std::string q = to_lower_copy(query);
  std::error_code ec;
  for (fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end; it != end; ++it)
  {
    const fs::path &p = it->path();
    if (fs::is_directory(p, ec))
      continue;
    if (fs::file_size(p, ec) > SIZE_CAP_BYTES)
      continue;
    std::ifstream f(p);
    if (!f)
      continue;
    std::string line;
    int lineno = 0;
    while (std::getline(f, line))
    {
      ++lineno;
      if (to_lower_copy(line).find(q) != std::string::npos)
      {
        results.push_back({p, lineno, line});
        break;
      }
    }
  }
  return results;
}

static int clamp(int v, int lo, int hi) { return (v < lo) ? lo : (v > hi ? hi : v); }

static std::optional<Match> search_dialog_and_select(const fs::path &base)
{
  std::string query = prompt_user("\033[36mfind:\033[0m ");
  if (query.empty())
    return std::nullopt;

  clear_screen();
  cursor_to(1, 1);
  std::cout << "Searching for \"" << query << "\"...\n"
            << std::flush;

  auto matches = find_in_files(base, query);
  if (matches.empty())
  {
    cursor_to(terminal_rows(), 1);
    clear_line();
    std::cout << "No matches. Press Enter..." << std::flush;
    std::string dummy;
    std::getline(std::cin, dummy);
    return std::nullopt;
  }

  int sel = 0, scroll = 0;
  while (true)
  {
    int rows = terminal_rows();
    int header = 2;
    int view = std::max(1, rows - header);
    cursor_to(1, 1);
    clear_screen();
    std::cout << "\033[36mMatches for \"" << query << "\" (" << matches.size()
              << "). Enter=open  q/ESC=back  ↑/↓ move\033[0m\n\n";

    int total = (int)matches.size();
    sel = clamp(sel, 0, total - 1);
    if (sel < scroll)
      scroll = sel;
    if (sel >= scroll + view)
      scroll = sel - (view - 1);
    scroll = clamp(scroll, 0, std::max(0, total - view));

    for (int i = scroll; i < std::min(scroll + view, total); ++i)
    {
      const auto &m = matches[i];
      std::string preview = m.preview;
      for (char &c : preview)
        if ((unsigned char)c < 0x20 && c != '\t')
          c = ' ';
      if ((int)preview.size() > 120)
        preview.erase(120);
      if (i == sel)
        std::cout << "\033[7m";
      std::cout << m.file.string() << ":" << m.line << "  -  " << preview << "\033[0m\n";
    }
    std::cout.flush();

    std::string k = read_key();
    if (k == "q" || k == "\x1b")
      return std::nullopt;
    if (k == "UP" || k == "k")
      sel = clamp(sel - 1, 0, total - 1);
    else if (k == "DOWN" || k == "j")
      sel = clamp(sel + 1, 0, total - 1);
    else if (k == "\n")
      return matches[sel];
  }
}

static std::vector<std::string> prev_frame;

static std::tuple<std::vector<std::pair<Node *, int>>, int, int>
draw(Node *root, int sel_index, int scroll)
{
  auto vis = visible_nodes(root);
  int total = (int)vis.size();
  int rows = terminal_rows();
  int header_rows = 3;
  int win_height = std::max(1, rows - header_rows);
  scroll = clamp(scroll, 0, std::max(0, total - win_height));

  std::vector<std::string> frame;
  frame.emplace_back(std::string("\033[36m") + HELP_LINE + "\033[0m");
  frame.emplace_back(std::string("\033[34mcwd: ") + fs::current_path().string() +
                     " | items: " + std::to_string(total) + "\033[0m");
  frame.emplace_back("");

  for (int i = scroll; i < std::min(scroll + win_height, total); ++i)
  {
    auto [node, depth] = vis[i];
    std::string prefix(depth * 2, ' ');
    bool isDir = node->isDir, exp = node->expanded;
    std::string marker = isDir ? (exp ? "[+]" : "[ ]") : "   ";
    std::string color = isDir ? "\033[36m" : "\033[37m";
    std::string line = prefix + marker + " " + node->name;
    if (i == sel_index)
      frame.push_back("\033[7m" + line + "\033[0m");
    else
      frame.push_back(color + line + "\033[0m");
  }

  while ((int)frame.size() < header_rows + win_height)
    frame.emplace_back("");
  for (int r = 0; r < (int)frame.size(); ++r)
  {
    if (r < (int)prev_frame.size() && prev_frame[r] == frame[r])
      continue;
    cursor_to(r + 1, 1);
    std::cout << frame[r];
    clear_line();
  }
  std::cout.flush();
  prev_frame.swap(frame);
  return {std::move(vis), scroll, win_height};
}

struct TermRestore
{
  TermRestore()
  {
    use_alt_screen(true);
    hide_cursor(true);
  }
  ~TermRestore()
  {
    std::cout << "\033[2J\033[H";
    hide_cursor(false);
    use_alt_screen(false);
    std::cout.flush();
  }
};

int main()
{
#if defined(_WIN32)
  enableAnsiOnWindows();
  SetConsoleOutputCP(CP_UTF8);
  SetConsoleCP(CP_UTF8);
#endif

  TermRestore _guard;
  Node root(fs::current_path());
  root.expanded = true;
  root.toggle();
  int sel_index = 0, scroll = 0;

  try
  {
    while (true)
    {
      auto [vis, cur_scroll, win_height] = draw(&root, sel_index, scroll);
      scroll = cur_scroll;
      int total = (int)vis.size();
      std::string ch = read_key();

      if (ch == "q" || ch == "\x1b")
        break;
      else if (ch == "UP" || ch == "k")
        sel_index = clamp(sel_index - 1, 0, total - 1);
      else if (ch == "DOWN" || ch == "j")
        sel_index = clamp(sel_index + 1, 0, total - 1);
      else if (ch == "RIGHT" || ch == "l")
      {
        if (total > 0)
        {
          auto [n, _] = vis[sel_index];
          if (n->isDir && !n->expanded)
            n->toggle();
          else if (n->isDir && !n->children.empty())
            sel_index = std::min(sel_index + 1, total - 1);
        }
      }
      else if (ch == "LEFT" || ch == "h")
      {
        if (total > 0)
        {
          auto [n, _] = vis[sel_index];
          if (n->isDir && n->expanded)
            n->toggle();
          else if (n->parent)
          {
            for (int i = 0; i < total; ++i)
              if (vis[i].first == n->parent)
              {
                sel_index = i;
                break;
              }
          }
        }
      }
      else if (ch == "\n")
      {
        if (total > 0)
        {
          auto [n, _] = vis[sel_index];
          if (n->isDir)
            n->toggle();
          else
          {
            {
              ScopedAltScreenPause pause;
              open_in_editor(n->path);
            }
            prev_frame.clear();
          }
        }
      }
      else if (ch == "\t")
      {
        if (total > 0)
        {
          auto [n, _] = vis[sel_index];
          if (n->isDir)
          {
            std::error_code ec;
            fs::current_path(n->path, ec);
            if (!ec)
            {
              root = Node(fs::current_path());
              root.expanded = true;
              root.toggle();
              sel_index = 0;
              scroll = 0;
              prev_frame.clear();
            }
          }
        }
      }
      else if (ch == "f")
      {
        auto maybe = search_dialog_and_select(fs::current_path());
        if (maybe)
        {
          {
            ScopedAltScreenPause pause;
            open_in_editor_at(maybe->file, maybe->line);
          }
        }
        prev_frame.clear();
      }
      else if (ch == "r")
      {
        root.children.clear();
        root.expanded = true;
        root.toggle();
        prev_frame.clear();
      }
      else if (ch == "g")
        sel_index = 0;
      else if (ch == "G")
        sel_index = total - 1;

      if (sel_index < scroll)
        scroll = sel_index;
      else if (sel_index >= scroll + win_height)
        scroll = sel_index - (win_height - 1);
    }
  }
  catch (...)
  {
  }

  if (const char *out = std::getenv("DIRT_OUT"))
  {
    try
    {
      std::ofstream f(out);
      if (f)
        f << fs::current_path().string();
    }
    catch (...)
    {
    }
  }
  return 0;
}
