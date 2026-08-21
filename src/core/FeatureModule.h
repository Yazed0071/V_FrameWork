#pragma once

#include <Windows.h>
#include <vector>
#include <mutex>


class IFeatureModule
{
public:
    virtual ~IFeatureModule() = default;


    virtual const char* GetName() const = 0;

    virtual bool Install(HMODULE hGame) = 0;

    virtual void Uninstall() = 0;
};

bool FeatureIsDisabled(const char* name);
bool FeatureAnyDisabled();

class FeatureModuleRegistry
{
public:

    static FeatureModuleRegistry& Instance();

    void Register(IFeatureModule* module);

    bool InstallAll(HMODULE hGame);

    void UninstallAll();

private:
    std::vector<IFeatureModule*> m_Modules;
    std::vector<IFeatureModule*> m_Installed;
    std::mutex m_Mutex;
};