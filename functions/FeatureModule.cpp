#include "pch.h"
#include "FeatureModule.h"
#include "log.h"

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

    for (IFeatureModule* module : m_Modules)
    {
        if (!module)
            continue;

        const bool ok = module->Install(hGame);
        Log("[FeatureModule] Install %s -> %s\n", module->GetName(), ok ? "OK" : "FAIL");

        if (!ok)
            allOk = false;
    }

    return allOk;
}

void FeatureModuleRegistry::UninstallAll()
{
    std::lock_guard<std::mutex> lock(m_Mutex);

    for (auto it = m_Modules.rbegin(); it != m_Modules.rend(); ++it)
    {
        IFeatureModule* module = *it;
        if (!module)
            continue;

        module->Uninstall();
        Log("[FeatureModule] Uninstall %s -> done\n", module->GetName());
    }
}