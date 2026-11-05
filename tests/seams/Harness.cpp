// SPDX-License-Identifier: GPL-3.0-or-later
// wxl-seams-test.exe: the fakes the seam files link against, and the checks in order.

#include "tests/seams/Harness.hpp"

#include "engine/hook/Registry.hpp"
#include "runtime/HookPoints.hpp"
#include "runtime/objects/Lifetime.hpp"
#include "wxl/offsets/CGObjectData.hpp"
#include "wxl/offsets/CGObject_C.hpp"

#include <cstring>
#include <vector>

namespace harness
{
    int                              g_failures = 0;
    FakeSource                       g_source;
    obj::Table                       g_table(g_source);
    std::map<std::string, Attached>  g_points;

    namespace
    {
        namespace off = wxl::offsets;

        std::vector<bool (*)()>& Installers()
        {
            static std::vector<bool (*)()> installers;
            return installers;
        }
    }

    FakeObject::FakeObject(uint64_t guid, uint32_t typeMask)
    {
        void* head = header;
        std::memcpy(bytes + off::CGObject_C::fields::m_obj, &head, sizeof(head));
        std::memcpy(header + off::CGObjectData::fields::m_guid, &guid, sizeof(guid));
        std::memcpy(header + off::CGObjectData::fields::m_type, &typeMask, sizeof(typeMask));
    }

    void* FakeSource::Find(uint64_t guid)
    {
        const auto it = resident.find(guid);
        return it == resident.end() ? nullptr : it->second;
    }

    uint64_t FakeSource::GuidOf(void* object)
    {
        uint64_t guid;
        std::memcpy(&guid, static_cast<FakeObject*>(object)->header + off::CGObjectData::fields::m_guid, sizeof(guid));
        return guid;
    }

    uint32_t FakeSource::TypeMaskOf(void* object)
    {
        uint32_t mask;
        std::memcpy(&mask, static_cast<FakeObject*>(object)->header + off::CGObjectData::fields::m_type, sizeof(mask));
        return mask;
    }

    void InstallFeatures()
    {
        for (auto install : Installers()) CHECK(install());
    }

    void RegisterInstaller(bool (*install)()) { Installers().push_back(install); }
}

// What the seam files call in the core, answered by the fakes.
namespace wxl::hook
{
    void RegisterFeature(const char*, bool enabled, bool (*install)(), Phase)
    {
        if (enabled) harness::RegisterInstaller(install);
    }
}

namespace wxl::runtime::hookpoints
{
    int AttachByName(const char* pointName, void* detour, void** original, int)
    {
        harness::g_points[pointName] = harness::Attached{ detour, original };
        return 1;
    }
}

namespace wxl::runtime::objects
{
    void Added(void* object) { Announce(harness::g_table, harness::g_source, object); }
    void Removed(void* object) { Withdraw(harness::g_table, harness::g_source, object); }

    WXL_Handle HandleOf(uint64_t guid)
    {
        WXL_Handle handle{};
        if (guid) harness::g_table.FromGuid(guid, &handle);
        return handle;
    }

    void* PointerOf(WXL_Handle handle, uint32_t typeMask)
    {
        void* object = nullptr;
        harness::g_table.Resolve(handle, typeMask, &object);
        return object;
    }
}

int main()
{
    harness::g_table.OnExpire([](WXL_Handle handle) { wxl::events::DropSubject(handle); });
    harness::InstallFeatures();
    harness::CheckLifetime();
    harness::CheckThunks();
    harness::CheckDamage();
    harness::CheckUnitAndChat();
    harness::CheckWorld();
    std::printf("%d failure(s)\n", harness::g_failures);
    return harness::g_failures == 0 ? 0 : 1;
}
