// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/storage/Providers.hpp"

#include "engine/storage/ArchiveName.hpp"

#include <atomic>
#include <mutex>
#include <string>

namespace wxl::storage
{
    namespace
    {
        /// Written under a lock, read without one: an item is complete before the count that shows it.
        template <class T>
        class AppendOnly
        {
        public:
            bool Add(T item)
            {
                std::lock_guard<std::mutex> lock(m_writers);
                const uint32_t count = m_count.load(std::memory_order_relaxed);
                if (count == kMaxRegistrations) return false;
                m_items[count] = std::move(item);
                m_count.store(count + 1, std::memory_order_release);
                return true;
            }

            /// Calls `claims` on each item in order; true at the first that answers true.
            template <class F>
            bool First(F&& claims) const
            {
                const uint32_t count = m_count.load(std::memory_order_acquire);
                for (uint32_t i = 0; i < count; ++i)
                    if (claims(m_items[i])) return true;
                return false;
            }

            void Clear()
            {
                std::lock_guard<std::mutex> lock(m_writers);
                m_count.store(0);
            }

        private:
            T                     m_items[kMaxRegistrations]{};
            std::atomic<uint32_t> m_count{ 0 };
            std::mutex            m_writers;
        };

        struct TransformEntry
        {
            std::string     suffix;
            WXL_TransformFn fn = nullptr;
        };

        AppendOnly<WXL_RedirectFn> g_redirects;
        AppendOnly<WXL_ProvideFn>  g_providers;
        AppendOnly<TransformEntry> g_transforms;

        void __cdecl SinkWrite(void* context, const void* data, unsigned int length)
        {
            auto*       out   = static_cast<std::vector<uint8_t>*>(context);
            const auto* bytes = static_cast<const uint8_t*>(data);
            out->insert(out->end(), bytes, bytes + length);
        }
    }

    bool AddRedirect(WXL_RedirectFn fn) { return fn && g_redirects.Add(fn); }

    bool AddProvider(WXL_ProvideFn fn) { return fn && g_providers.Add(fn); }

    bool AddTransform(const char* suffix, WXL_TransformFn fn)
    {
        if (!fn || !suffix || !*suffix) return false;
        return g_transforms.Add(TransformEntry{ suffix, fn });
    }

    bool Redirect(const char* name, char (&out)[kMaxRedirectName])
    {
        return g_redirects.First([&](WXL_RedirectFn fn) {
            out[0] = '\0';
            if (!fn(name, out, kMaxRedirectName)) return false;
            out[kMaxRedirectName - 1] = '\0';
            return out[0] != '\0';
        });
    }

    bool Provide(const char* name, std::vector<uint8_t>& out)
    {
        const WXL_ByteSink sink{ &out, &SinkWrite };
        const bool claimed = g_providers.First([&](WXL_ProvideFn fn) {
            out.clear();
            return fn(name, &sink) != 0;
        });
        if (!claimed) out.clear();
        return claimed;
    }

    bool HasTransform(std::string_view name)
    {
        return g_transforms.First([&](const TransformEntry& t) { return EndsWithCI(name, t.suffix); });
    }

    bool Transform(const char* name, const uint8_t* raw, uint32_t size, std::vector<uint8_t>& out)
    {
        const std::string_view view(name);
        const WXL_ByteSink     sink{ &out, &SinkWrite };
        const bool claimed = g_transforms.First([&](const TransformEntry& t) {
            if (!EndsWithCI(view, t.suffix)) return false;
            out.clear();
            return t.fn(name, raw, size, &sink) != 0;
        });
        if (!claimed) out.clear();
        return claimed;
    }

    void Reset()
    {
        g_redirects.Clear();
        g_providers.Clear();
        g_transforms.Clear();
    }
}
