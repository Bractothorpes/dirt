#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <stdexcept>

namespace fs = std::filesystem;

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
static const std::string INSTALL_PATH = "C:\\Program Files\\dirt";
#else
static const std::string INSTALL_PATH = "/usr/local/dirt";
#endif

void copy_recursive(const fs::path &src, const fs::path &dst)
{
    if (!fs::exists(src))
        throw std::runtime_error("Source folder not found: " + src.string());
    fs::create_directories(dst);
    for (auto &p : fs::recursive_directory_iterator(src))
    {
        fs::path rel = fs::relative(p.path(), src);
        fs::path target = dst / rel;
        if (fs::is_directory(p))
            fs::create_directories(target);
        else
            fs::copy_file(p, target, fs::copy_options::overwrite_existing);
    }
}

#if defined(_WIN32)

bool create_windows_wrappers()
{
    std::string dir = INSTALL_PATH;
    fs::path exe = fs::path(dir) / "dirt-bin.exe";

    {
        fs::path pw = fs::path(dir) / "dirt.ps1";
        std::ofstream f(pw);
        if (!f)
            return false;
        f << "$tmp = [IO.Path]::GetTempFileName()\n";
        f << "setx DIRT_OUT $tmp > $null\n";
        f << "\"" << exe.string() << "\" $args\n";
        f << "if (Test-Path $tmp) {\n";
        f << "  $d = Get-Content $tmp\n";
        f << "  if (Test-Path $d) { Set-Location $d }\n";
        f << "  Remove-Item $tmp\n";
        f << "}\n";
    }

    {
        fs::path cmd = fs::path(dir) / "dirt.cmd";
        std::ofstream f(cmd);
        if (!f)
            return false;
        f << "@echo off\n";
        f << "set TMPFILE=%TEMP%\\dirt_cd.txt\n";
        f << "set DIRT_OUT=%TMPFILE%\n";
        f << "\"" << exe.string() << "\" %*\n";
        f << "if exist \"%TMPFILE%\" (\n";
        f << "  set /p d=<\"%TMPFILE%\"\n";
        f << "  cd /d \"%d%\"\n";
        f << "  del \"%TMPFILE%\"\n";
        f << ")\n";
    }
    return true;
}

bool add_to_path_windows(const std::string &dir)
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Environment", 0, KEY_READ | KEY_WRITE, &hKey) != ERROR_SUCCESS)
        return false;

    char buf[8192];
    DWORD size = sizeof(buf);
    std::string pathValue;
    if (RegGetValueA(hKey, nullptr, "Path", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        pathValue = buf;

    if (pathValue.find(dir) == std::string::npos)
    {
        if (!pathValue.empty() && pathValue.back() != ';')
            pathValue += ';';
        pathValue += dir;
        RegSetValueExA(hKey, "Path", 0, REG_EXPAND_SZ, (const BYTE *)pathValue.c_str(), (DWORD)pathValue.size() + 1);
        SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM) "Environment", SMTO_ABORTIFHUNG, 5000, nullptr);
        RegCloseKey(hKey);
        return true;
    }
    RegCloseKey(hKey);
    return false;
}

#else

static bool chmod_0644(const fs::path &p) { return ::chmod(p.c_str(), 0644) == 0; }
static bool chmod_0755(const fs::path &p) { return ::chmod(p.c_str(), 0755) == 0; }

bool write_unix_function(const std::string &dir)
{
    const char *home = std::getenv("HOME");
    if (!home)
        return false;

    fs::path cfgdir = fs::path(home) / ".config" / "dirt";
    std::error_code ec;
    fs::create_directories(cfgdir, ec);

    fs::path fn = cfgdir / "dirt.sh";
    std::ofstream f(fn);
    if (!f)
        return false;

    f << "dirt() {\n";
    f << "  local tmp\n";
    f << "  tmp=\"$(mktemp)\" || return\n";
    f << "  DIRT_OUT=\"$tmp\" \"" << dir << "/dirt-bin\" \"$@\"\n";
    f << "  if [ -s \"$tmp\" ]; then\n";
    f << "    cd -- \"$(cat \"$tmp\")\" 2>/dev/null || true\n";
    f << "  fi\n";
    f << "  rm -f \"$tmp\"\n";
    f << "}\n";

    chmod_0644(fn);
    return true;
}

bool add_to_shell_rc_unix(const std::string &dir)
{
    const char *home = std::getenv("HOME");
    if (!home)
        return false;

    {
        fs::path loader = "/usr/local/bin/dirt";
        std::ofstream f(loader);
        if (f)
        {
            f << "#!/bin/sh\n";
            f << "echo \"Note: use the 'dirt' shell function for directory jumps.\"\n";
            f << "exec \"" << dir << "/dirt-bin\" \"$@\"\n";
            f.close();
            chmod_0755(loader);
        }
    }

    std::vector<std::string> rcs = {".bashrc", ".zshrc", ".profile"};
    for (const auto &rc : rcs)
    {
        fs::path rc_path = fs::path(home) / rc;
        std::ofstream touch(rc_path, std::ios::app);
    }

    const std::string snippet = "\n# Dirt function\n[ -f \"$HOME/.config/dirt/dirt.sh\" ] && . \"$HOME/.config/dirt/dirt.sh\"\n";

    for (const auto &rc : rcs)
    {
        fs::path rc_path = fs::path(home) / rc;

        std::ifstream in(rc_path);
        std::string line;
        bool has_line = false;
        while (std::getline(in, line))
        {
            if (line.find(". \"$HOME/.config/dirt/dirt.sh\"") != std::string::npos ||
                line.find("source \"$HOME/.config/dirt/dirt.sh\"") != std::string::npos)
            {
                has_line = true;
                break;
            }
        }
        in.close();

        if (!has_line)
        {
            std::ofstream out(rc_path, std::ios::app);
            if (out)
                out << snippet;
        }
    }
    return true;
}

#endif

int main()
{
    try
    {
        fs::path src = "dirt";
        fs::path dest = INSTALL_PATH;

        std::cout << "Installing Dirt to: " << dest << std::endl;
        copy_recursive(src, dest);

#if defined(_WIN32)
        fs::rename(dest / "dirt.exe", dest / "dirt-bin.exe");

        create_windows_wrappers();

        if (add_to_path_windows(dest.string()))
            std::cout << "Added to PATH.\n";
        else
            std::cout << "PATH unchanged (already configured).\n";

        std::cout << "\nOpen a NEW PowerShell and run: dirt.ps1\n";
        std::cout << "Open a NEW cmd.exe and run: dirt.cmd\n";
        system("pause");

#else
        fs::rename(dest / "dirt", dest / "dirt-bin");

        bool ok_fn = write_unix_function(dest.string());
        bool ok_rc = add_to_shell_rc_unix(dest.string());

        if (ok_fn && ok_rc)
        {
            std::cout << "Installed shell function 'dirt'. Restart your shell or run:\n"
                      << "  . \"$HOME/.config/dirt/dirt.sh\"\n"
                      << "Then use: dirt\n";
        }
        else
        {
            std::cout << "Could not auto-install shell function.\n"
                      << "Manually add to your shell rc:\n"
                      << "  [ -f \"$HOME/.config/dirt/dirt.sh\" ] && . \"$HOME/.config/dirt/dirt.sh\"\n";
        }
#endif

        std::cout << "\nInstall complete.\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
