// SPDX-License-Identifier: MIT
// Copyright (c) 2026 WarcraftXL
//
// The wxl.relay service. Requests, answers and cancellations are queued as they are made, from
// any thread, and handed to the mods they are for at the next Pump, on the main thread, each call
// into a mod under the crash guard. Nothing is called with the lock held, so a mod may call the
// service again from inside what it is handed.

#include "services/RelayService.hpp"

#include "services/ModCall.hpp"

#include "common/Log.hpp"
#include "runtime/Core.hpp"

#include "wxl/host/mod_host.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace
{
    using wxl::core::services::CallMod;

    constexpr char   kRelayTag[]   = "relay";
    constexpr size_t kNameMax = 127;

    /// How many rounds one Pump makes: what a round's calls queue is handed in the next.
    constexpr int kRounds = 4;

    char Lower(char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); }

    std::string Key(std::string_view name)
    {
        std::string key(name);
        for (char& c : key) c = Lower(c);
        return key;
    }

    uint32_t Asking()
    {
        const wxl::host::ModHost* host = wxl::core::Host();
        return host ? host->CurrentMod() : 0u;
    }

    const char* ModId(uint32_t mod)
    {
        const wxl::host::ModHost*   host   = wxl::core::Host();
        const wxl::host::LoadedMod* loaded = host ? host->ModOf(mod) : nullptr;
        return loaded ? loaded->id.c_str() : "a mod";
    }

    // --- values, copied ----------------------------------------------------------------------

    struct Owned
    {
        uint32_t    type   = WXL_VALUE_NIL;
        double      number = 0.0;
        wxl_bool    flag   = WXL_FALSE;
        std::string text;
    };

    using Values = std::vector<Owned>;

    Values Copy(const wxl_value* values, uint32_t count)
    {
        Values copy;
        if (!values) return copy;
        copy.reserve(count);
        for (uint32_t i = 0; i < count; ++i)
        {
            Owned owned;
            owned.type = values[i].type;
            switch (values[i].type)
            {
            case WXL_VALUE_NUMBER: owned.number = values[i].as.number; break;
            case WXL_VALUE_BOOL: owned.flag = values[i].as.boolean ? WXL_TRUE : WXL_FALSE; break;
            case WXL_VALUE_STRING:
                if (values[i].as.string.data) owned.text.assign(values[i].as.string.data, values[i].as.string.size);
                break;
            default: owned.type = WXL_VALUE_NIL; break;
            }
            copy.push_back(std::move(owned));
        }
        return copy;
    }

    /// The values as the boundary carries them, pointing into @p values.
    std::vector<wxl_value> View(const Values& values)
    {
        std::vector<wxl_value> view(values.size());
        for (size_t i = 0; i < values.size(); ++i)
        {
            view[i].type = values[i].type;
            switch (values[i].type)
            {
            case WXL_VALUE_NUMBER: view[i].as.number = values[i].number; break;
            case WXL_VALUE_BOOL: view[i].as.boolean = values[i].flag; break;
            case WXL_VALUE_STRING:
                view[i].as.string = wxl_str{values[i].text.c_str(), static_cast<uint32_t>(values[i].text.size())};
                break;
            default: break;
            }
        }
        return view;
    }

    // --- what is published and asked --------------------------------------------------------

    struct Endpoint
    {
        std::string        name;
        uint32_t           mod = 0;
        wxl_relay_endpoint table{};
    };

    using EndpointPtr = std::shared_ptr<Endpoint>;

    enum class State
    {
        Queued,    ///< not yet handed to its service
        Delivered, ///< handed, waiting for its answer
        Answered,  ///< answered, the answer on its way
    };

    struct Request
    {
        wxl_request        id     = 0;
        uint32_t           caller = 0;
        wxl_relay_reply_fn reply  = nullptr;
        void*              user   = nullptr;
        std::string        key;
        std::string        method;
        Values             args;
        EndpointPtr        to;
        State              state = State::Queued;
    };

    enum class Action
    {
        Deliver,
        Reply,
        Cancelled,
    };

    struct Pending
    {
        Action      what   = Action::Deliver;
        wxl_request id     = 0;
        wxl_result  result = WXL_OK;
        Values      values;
        EndpointPtr endpoint;
    };

    std::mutex                                   g_lock;
    std::unordered_map<std::string, EndpointPtr> g_endpoints; ///< by Key(name)
    std::unordered_map<wxl_request, Request>     g_requests;
    std::deque<Pending>                          g_pending;
    std::atomic<uint64_t>                        g_nextRequest{1};

    /// Queues a request's failure: nobody publishes its name, or its service went. Under the lock.
    void FailLocked(wxl_request id, wxl_result result)
    {
        Pending pending;
        pending.what   = Action::Reply;
        pending.id     = id;
        pending.result = result;
        g_pending.push_back(std::move(pending));
    }

    /// Takes a service out and fails what it was asked and had not answered. Under the lock.
    void WithdrawLocked(const EndpointPtr& endpoint)
    {
        g_endpoints.erase(Key(endpoint->name));
        for (auto& [id, request] : g_requests)
        {
            const bool queuedFor    = request.state == State::Queued && request.key == Key(endpoint->name);
            const bool deliveredTo  = request.state == State::Delivered && request.to == endpoint;
            if (!queuedFor && !deliveredTo) continue;
            request.state = State::Answered;
            FailLocked(id, WXL_E_NOT_FOUND);
        }
    }

    /// Whether the calling code may answer for a service: its own mod's, or code that is no
    /// mod's at all (a thread of the mod's own, which the host does not know).
    bool MayAct(uint32_t owner)
    {
        const uint32_t asking = Asking();
        return asking == 0 || asking == owner;
    }

    // --- the table ---------------------------------------------------------------------------

    bool Nameable(wxl_str name) { return name.data && name.size && name.size <= kNameMax; }

    wxl_result WXL_CALL Publish(wxl_str name, const wxl_relay_endpoint* endpoint)
    {
        if (!Nameable(name) || !endpoint || endpoint->struct_size < offsetof(wxl_relay_endpoint, cancelled) ||
            !endpoint->request)
            return WXL_E_INVALID;
        const uint32_t    mod = Asking();
        const std::string key = Key(std::string_view(name.data, name.size));
        auto              published = std::make_shared<Endpoint>();
        published->name.assign(name.data, name.size);
        published->mod = mod;
        std::memcpy(&published->table, endpoint, std::min<size_t>(endpoint->struct_size, sizeof(wxl_relay_endpoint)));
        published->table.struct_size = sizeof(wxl_relay_endpoint);
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto known = g_endpoints.find(key);
            if (known != g_endpoints.end() && known->second->mod != mod) return WXL_E_INVALID;
            g_endpoints[key] = published;
        }
        WLOG_INFO(kRelayTag, "mod %s publishes %.*s", ModId(mod), static_cast<int>(name.size), name.data);
        return WXL_OK;
    }

    wxl_result WXL_CALL Unpublish(wxl_str name)
    {
        if (!Nameable(name)) return WXL_E_INVALID;
        const std::string key = Key(std::string_view(name.data, name.size));
        {
            std::lock_guard<std::mutex> lock(g_lock);
            const auto known = g_endpoints.find(key);
            if (known == g_endpoints.end() || known->second->mod != Asking()) return WXL_E_NOT_FOUND;
            const EndpointPtr endpoint = known->second;
            WithdrawLocked(endpoint);
        }
        WLOG_INFO(kRelayTag, "mod %s takes %.*s back", ModId(Asking()), static_cast<int>(name.size), name.data);
        return WXL_OK;
    }

    wxl_bool WXL_CALL Published(wxl_str name)
    {
        if (!Nameable(name)) return WXL_FALSE;
        std::lock_guard<std::mutex> lock(g_lock);
        return g_endpoints.count(Key(std::string_view(name.data, name.size))) ? WXL_TRUE : WXL_FALSE;
    }

    wxl_result WXL_CALL Call(wxl_str name, wxl_str method, const wxl_value* args, uint32_t count,
                             wxl_relay_reply_fn reply, void* user, wxl_request* out)
    {
        if (!Nameable(name) || !reply || !out || (count && !args)) return WXL_E_INVALID;
        Request request;
        request.id     = g_nextRequest.fetch_add(1, std::memory_order_relaxed);
        request.caller = Asking();
        request.reply  = reply;
        request.user   = user;
        request.key    = Key(std::string_view(name.data, name.size));
        if (method.data) request.method.assign(method.data, method.size);
        request.args = Copy(args, count);
        const wxl_request id = request.id;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_requests.emplace(id, std::move(request));
            Pending pending;
            pending.what = Action::Deliver;
            pending.id   = id;
            g_pending.push_back(std::move(pending));
        }
        *out = id;
        return WXL_OK;
    }

    wxl_result Respond(wxl_request id, wxl_result result, Values values)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const auto it = g_requests.find(id);
        if (it == g_requests.end() || it->second.state != State::Delivered || !it->second.to ||
            !MayAct(it->second.to->mod))
            return WXL_E_NOT_FOUND;
        it->second.state = State::Answered;
        Pending pending;
        pending.what   = Action::Reply;
        pending.id     = id;
        pending.result = result;
        pending.values = std::move(values);
        g_pending.push_back(std::move(pending));
        return WXL_OK;
    }

    wxl_result WXL_CALL Answer(wxl_request request, const wxl_value* values, uint32_t count)
    {
        if (count && !values) return WXL_E_INVALID;
        return Respond(request, WXL_OK, Copy(values, count));
    }

    wxl_result WXL_CALL Fail(wxl_request request, wxl_str reason)
    {
        Values values(1);
        values[0].type = WXL_VALUE_STRING;
        if (reason.data) values[0].text.assign(reason.data, reason.size);
        return Respond(request, WXL_E_FAILED, std::move(values));
    }

    wxl_result WXL_CALL Cancel(wxl_request request)
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const auto it = g_requests.find(request);
        if (it == g_requests.end() || !MayAct(it->second.caller)) return WXL_E_NOT_FOUND;
        if (it->second.state == State::Delivered && it->second.to)
        {
            Pending pending;
            pending.what     = Action::Cancelled;
            pending.id       = request;
            pending.endpoint = it->second.to;
            g_pending.push_back(std::move(pending));
        }
        g_requests.erase(it);
        return WXL_OK;
    }

    const wxl_relay_api g_relay = {
        sizeof(wxl_relay_api), &Publish, &Unpublish, &Published, &Call, &Answer, &Fail, &Cancel,
    };

    /// Hands one queued thing to the mod it is for.
    void Hand(Pending& pending)
    {
        switch (pending.what)
        {
        case Action::Deliver:
        {
            EndpointPtr            endpoint;
            std::string            method;
            std::vector<wxl_value> args;
            Values                 kept;
            uint32_t               caller = 0;
            wxl_relay_reply_fn     reply  = nullptr;
            void*                  user   = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                const auto it = g_requests.find(pending.id);
                if (it == g_requests.end() || it->second.state != State::Queued) return;
                Request&   request = it->second;
                const auto found   = g_endpoints.find(request.key);
                if (found == g_endpoints.end())
                {
                    caller = request.caller;
                    reply  = request.reply;
                    user   = request.user;
                    g_requests.erase(it);
                }
                else
                {
                    endpoint      = found->second;
                    request.state = State::Delivered;
                    request.to    = endpoint;
                    method        = request.method;
                    kept          = request.args;
                }
            }
            if (!endpoint)
            {
                // Nobody publishes the name: the caller is told, as an answer comes.
                CallMod(caller, kRelayTag, "its wxl.relay answer function",
                        [&] { reply(user, pending.id, WXL_E_NOT_FOUND, nullptr, 0); });
                return;
            }
            args = View(kept);
            CallMod(endpoint->mod, kRelayTag, "its wxl.relay service", [&] {
                endpoint->table.request(endpoint->table.user, pending.id,
                                        wxl_str{method.c_str(), static_cast<uint32_t>(method.size())},
                                        args.empty() ? nullptr : args.data(), static_cast<uint32_t>(args.size()));
            });
            return;
        }
        case Action::Reply:
        {
            uint32_t           caller = 0;
            wxl_relay_reply_fn reply  = nullptr;
            void*              user   = nullptr;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                const auto it = g_requests.find(pending.id);
                if (it == g_requests.end()) return;
                caller = it->second.caller;
                reply  = it->second.reply;
                user   = it->second.user;
                g_requests.erase(it);
            }
            const std::vector<wxl_value> values = View(pending.values);
            CallMod(caller, kRelayTag, "its wxl.relay answer function", [&] {
                reply(user, pending.id, pending.result, values.empty() ? nullptr : values.data(),
                      static_cast<uint32_t>(values.size()));
            });
            return;
        }
        case Action::Cancelled:
        {
            const EndpointPtr& endpoint = pending.endpoint;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                const auto found = g_endpoints.find(Key(endpoint->name));
                if (found == g_endpoints.end() || found->second != endpoint) return;
            }
            if (!endpoint->table.cancelled) return;
            CallMod(endpoint->mod, kRelayTag, "its wxl.relay cancel function",
                    [&] { endpoint->table.cancelled(endpoint->table.user, pending.id); });
            return;
        }
        }
    }
}

namespace wxl::core::services
{
    const wxl_relay_api& RelayApi() { return g_relay; }

    namespace relay
    {
        void Pump()
        {
            for (int round = 0; round < kRounds; ++round)
            {
                std::deque<Pending> ready;
                {
                    std::lock_guard<std::mutex> lock(g_lock);
                    if (g_pending.empty()) return;
                    ready.swap(g_pending);
                }
                for (Pending& pending : ready) Hand(pending);
            }
        }

        void ForgetMod(uint32_t mod)
        {
            std::lock_guard<std::mutex> lock(g_lock);
            std::vector<EndpointPtr> gone;
            for (const auto& [key, endpoint] : g_endpoints)
                if (endpoint->mod == mod) gone.push_back(endpoint);
            for (const EndpointPtr& endpoint : gone)
            {
                WithdrawLocked(endpoint);
                WLOG_INFO(kRelayTag, "mod %s: %s taken back", ModId(mod), endpoint->name.c_str());
            }
            for (auto it = g_requests.begin(); it != g_requests.end();)
            {
                Request& request = it->second;
                if (request.caller != mod)
                {
                    ++it;
                    continue;
                }
                if (request.state == State::Delivered && request.to && request.to->mod != mod)
                {
                    Pending pending;
                    pending.what     = Action::Cancelled;
                    pending.id       = request.id;
                    pending.endpoint = request.to;
                    g_pending.push_back(std::move(pending));
                }
                it = g_requests.erase(it);
            }
        }

        void Clear()
        {
            std::lock_guard<std::mutex> lock(g_lock);
            g_endpoints.clear();
            g_requests.clear();
            g_pending.clear();
        }
    }
}
