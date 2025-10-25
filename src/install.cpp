#include <iostream>
#include <filesystem>
#include <fstream>
#include <cstdlib>

#if defined(_WIN32)
#include <windows.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

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
        SendMessageTimeoutA(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM) "Environment",
                            SMTO_ABORTIFHUNG, 5000, nullptr);
        RegCloseKey(hKey);
        return true;
    }
    RegCloseKey(hKey);
    return false;
}
#else
bool add_to_path_unix(const std::string &dir)
{
    const char *home = std::getenv("HOME");
    if (!home)
        return false;

    std::vector<std::string> shells = {
        ".bashrc", ".zshrc", ".profile"};

    for (const auto &rc : shells)
    {
        fs::path rc_path = fs::path(home) / rc;
        std::ofstream f(rc_path, std::ios::app);
        if (!f)
            continue;
        f << "\n# Added by Dirt installer\n";
        f << "export PATH=\"$PATH:" << dir << "\"\n";
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
        if (add_to_path_windows(dest.string()))
            std::cout << "PATH updated successfully.\n";
        else
            std::cout << "PATH already contained Dirt or could not be updated.\n";
#else
        if (add_to_path_unix(dest.string()))
            std::cout << "PATH entries added to ~/.bashrc, ~/.zshrc, and ~/.profile.\n";
        else
            std::cout << "Failed to modify PATH automatically.\n";
#endif

        std::cout << "\nInstallation complete!\n";
#if defined(_WIN32)
        std::cout << "Open a new Command Prompt and type: dirt\n";
        system("pause");
#else
        std::cout << "Restart your terminal and type: dirt\n";
#endif
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
