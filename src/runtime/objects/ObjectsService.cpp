// SPDX-License-Identifier: GPL-3.0-or-later
// The wxl.objects service over the client's object manager, and the lifetime events it raises.

#include "runtime/objects/Objects.hpp"
#include "runtime/objects/Lifetime.hpp"

#include "engine/events/Event.hpp"
#include "runtime/extensions/Services.hpp"
#include "wxl/Common.hpp"
#include "wxl/game/World.hpp"

namespace wxl::runtime::objects
{
    namespace
    {
        /// The object manager lives in the main thread's TLS: asked from elsewhere, it faults.
        class ClientSource final : public Source
        {
        public:
            bool InWorld() override
            {
                uint64_t guid = 0;
                return ActivePlayer(&guid) && guid != 0;
            }

            void* Find(uint64_t guid) override
            {
                void* object = nullptr;
                return Lookup(guid, &object) ? object : nullptr;
            }

            uint64_t GuidOf(void* object) override { return game::world::Guid(object); }
            uint32_t TypeMaskOf(void* object) override { return game::world::TypeMask(object); }

        private:
            static __declspec(noinline) bool ActivePlayer(uint64_t* guid)
            {
                WXL_SEH_TRY(seh::Catch::AccessViolation) { *guid = game::world::ActivePlayerGuid(); return true; }
                WXL_SEH_EXCEPT { return false; }
            }

            static __declspec(noinline) bool Lookup(uint64_t guid, void** object)
            {
                WXL_SEH_TRY(seh::Catch::AccessViolation)
                {
                    *object = game::world::ResolveObject(guid, game::world::kTypeMaskObject);
                    return true;
                }
                WXL_SEH_EXCEPT { return false; }
            }
        };

        ClientSource g_source;
        Table        g_table(g_source);

        int __cdecl ApiFromGuid(uint64_t guid, WXL_Handle* out) { return g_table.FromGuid(guid, out); }
        int __cdecl ApiFromPointer(void* object, WXL_Handle* out) { return g_table.FromPointer(object, out); }
        int __cdecl ApiResolve(WXL_Handle handle, uint32_t typeMask, void** out)
        {
            return g_table.Resolve(handle, typeMask, out);
        }
        int __cdecl ApiGuid(WXL_Handle handle, uint64_t* out) { return g_table.Guid(handle, out); }

        const WXL_ObjectsApi g_api = { sizeof(WXL_ObjectsApi), WXL_OBJECTS_API_VERSION,
                                       &ApiFromGuid, &ApiFromPointer, &ApiResolve, &ApiGuid };
    }

    void Publish()
    {
        g_table.OnExpire([](WXL_Handle handle) { events::DropSubject(handle); });
        extensions::PublishInterface(WXL_OBJECTS_API_NAME, WXL_OBJECTS_API_VERSION, const_cast<WXL_ObjectsApi*>(&g_api));
    }

    void Added(void* object) { Announce(g_table, g_source, object); }

    void Removed(void* object) { Withdraw(g_table, g_source, object); }

    WXL_Handle HandleOf(uint64_t guid)
    {
        WXL_Handle handle{};
        if (guid) g_table.FromGuid(guid, &handle);
        return handle;
    }

    void* PointerOf(WXL_Handle handle, uint32_t typeMask)
    {
        void* object = nullptr;
        g_table.Resolve(handle, typeMask, &object);
        return object;
    }
}
