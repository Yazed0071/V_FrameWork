#include "pch.h"
#include "FeatureModule.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>

namespace
{
    std::string NormalizeModuleName(const char* raw)
    {
        std::string in(raw ? raw : "");
        std::size_t b = 0;
        std::size_t e = in.size();
        while (b < e && std::isspace(static_cast<unsigned char>(in[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(in[e - 1])))
            --e;
        std::string out = in.substr(b, e - b);
        std::transform(out.begin(), out.end(), out.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return out;
    }

    const std::unordered_set<std::string>& DisabledModuleNames()
    {
        static const std::unordered_set<std::string> s_Disabled = []
        {
            std::unordered_set<std::string> set;

            char path[MAX_PATH]{};
            if (!GetModuleFileNameA(nullptr, path, MAX_PATH))
                return set;
            char* lastSlash = std::strrchr(path, '\\');
            if (!lastSlash)
                return set;
            *(lastSlash + 1) = '\0';
            strcat_s(path, "mod\\V_FrameWork\\disabled_modules.txt");

            FILE* f = nullptr;
            if (fopen_s(&f, path, "r") != 0 || !f)
                return set;

            char line[512];
            while (fgets(line, sizeof(line), f))
            {
                if (line[0] == '#')
                    continue;
                std::string name = NormalizeModuleName(line);
                if (!name.empty())
                    set.insert(name);
            }
            fclose(f);
            return set;
        }();
        return s_Disabled;
    }
}

bool FeatureIsDisabled(const char* name)
{
    if (!name || !*name)
        return false;
    const std::unordered_set<std::string>& disabled = DisabledModuleNames();
    return !disabled.empty() && disabled.count(NormalizeModuleName(name)) != 0;
}

bool FeatureAnyDisabled()
{
    return !DisabledModuleNames().empty();
}

FeatureModuleRegistry& FeatureModuleRegistry::Instance()
{
    static FeatureModuleRegistry s_Instance;
    return s_Instance;
}


void FeatureModuleRegistry::Register(IFeatureModule* module)
{
    if (!module)
        return;

    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Modules.push_back(module);
}

bool FeatureModuleRegistry::InstallAll(HMODULE hGame)
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    bool allOk = true;

    const std::unordered_set<std::string>& disabled = DisabledModuleNames();
    if (!disabled.empty())
        Log("[FeatureModule] disabled_modules.txt lists %zu module(s) to skip this "
            "boot; every skipped module is reported below as SKIPPED. Delete the file "
            "or prefix a line with # to bring a module back.\n",
            disabled.size());

    for (IFeatureModule* module : m_Modules)
    {
        if (!module)
            continue;

        const char* moduleName = module->GetName();
        if (!disabled.empty() && disabled.count(NormalizeModuleName(moduleName)) != 0)
        {
            Log("[FeatureModule] SKIPPED %s (listed in disabled_modules.txt)\n",
                moduleName ? moduleName : "<unnamed>");
            continue;
        }

        const bool ok = module->Install(hGame);
        if (ok)
            m_Installed.push_back(module);
#ifdef _DEBUG
        Log("[FeatureModule] Install %s -> %s\n", module->GetName(), ok ? "OK" : "FAIL");
#else
        if (!ok)
            Log("[FeatureModule] Install %s -> %s\n", module->GetName(), ok ? "OK" : "FAIL");
#endif

        if (!ok)
            allOk = false;
    }

    return allOk;
}

void FeatureModuleRegistry::UninstallAll()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    for (auto it = m_Installed.rbegin(); it != m_Installed.rend(); ++it)
    {
        IFeatureModule* module = *it;
        if (!module)
            continue;

        module->Uninstall();
    }

    m_Installed.clear();
}
