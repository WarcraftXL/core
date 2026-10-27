// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The scene's models animated on the runtime's threads: see Animate.hpp.

#include "tuning/Animate.hpp"

#include "common/Log.hpp"
#include "runtime/Session.hpp"
#include "runtime/Settings.hpp"
#include "binding/Binding.hpp"

#include <windows.h>
#include <intrin.h>

#include <vector>

namespace wxl::core::tuning
{
    namespace
    {
        constexpr const char* kTag = "engine";
        /// Spins of a waiting thread before it sleeps: a few microseconds, about what a wake costs.
        constexpr uint32_t kSpins = 4000;
    }

    FloatState CaptureFloat()
    {
        FloatState state;
#if defined(_M_IX86)
        uint16_t control = 0;
        __asm__ volatile("fnstcw %0" : "=m"(control));
        state.control = control;
#endif
        uint32_t mxcsr = 0;
        __asm__ volatile("stmxcsr %0" : "=m"(mxcsr));
        state.mxcsr = mxcsr;
        return state;
    }

    void LoadFloat(const FloatState& state)
    {
#if defined(_M_IX86)
        const uint16_t control = static_cast<uint16_t>(state.control);
        __asm__ volatile("fldcw %0" : : "m"(control));
#endif
        const uint32_t mxcsr = state.mxcsr;
        __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
    }

    // --- The pool ---------------------------------------------------------------------------------

    bool Pool::Start(uint32_t workers)
    {
        if (workers > 15) workers = 15;
        if (workers <= workers_) return true;
        for (uint32_t i = workers_; i < workers; ++i)
        {
            HANDLE thread = CreateThread(nullptr, 0, &Pool::Main, this, 0, nullptr);
            if (!thread)
            {
                // The ones started wait for a batch that never comes: harmless, and never more.
                WLOG_WARN(kTag, "a thread of the animate pool could not start (error %lu)", GetLastError());
                return false;
            }
            CloseHandle(thread);
        }
        // Each worker reads its starting floating point state before it waits for work.
        while (static_cast<uint32_t>(started_) < workers) SwitchToThread();
        workers_ = workers;
        return true;
    }

    unsigned long __stdcall Pool::Main(void* self)
    {
        Pool&          pool  = *static_cast<Pool*>(self);
        const uint32_t index = static_cast<uint32_t>(InterlockedIncrement(&pool.started_)) - 1;
        if (index == 0) pool.fresh_ = CaptureFloat();
        uint32_t seen = 0;
        for (;;)
        {
            uint32_t generation = pool.generation_;
            for (uint32_t spin = 0; generation == seen && spin < kSpins; ++spin)
            {
                __asm__ volatile("pause");
                generation = pool.generation_;
            }
            while (generation == seen)
            {
                WaitOnAddress(const_cast<uint32_t*>(&pool.generation_), &seen, sizeof seen, INFINITE);
                generation = pool.generation_;
            }
            seen = generation;
            if (index < pool.helpers_) pool.Work(generation);
        }
    }

    void Pool::Work(uint32_t generation)
    {
        // A job is claimed with the batch's number in the same word, so a thread that wakes late
        // never takes a job of a batch that has moved on.
        const FloatState own   = CaptureFloat();
        int              state = -1; // which of the batch's states is loaded: 0 first, 1 second
        for (;;)
        {
            const int64_t cursor = cursor_;
            if (static_cast<uint32_t>(static_cast<uint64_t>(cursor) >> 32) != generation) break;
            const uint32_t index = static_cast<uint32_t>(cursor);
            if (index >= count_) break;
            if (InterlockedCompareExchange64(&cursor_, cursor + 1, cursor) != cursor) continue;
            const int wanted = index < split_ ? 0 : 1;
            if (wanted != state)
            {
                LoadFloat(wanted == 0 ? first_ : second_);
                state = wanted;
            }
            job_(context_, index);
            InterlockedIncrement(&done_);
        }
        if (state >= 0) LoadFloat(own);
    }

    void Pool::Run(Job job, void* context, uint32_t count, uint32_t split, const FloatState& first, const FloatState& second,
                   uint32_t helpers)
    {
        if (!count) return;
        helpers_ = helpers < workers_ ? helpers : workers_;
        job_     = job;
        context_ = context;
        count_   = count;
        split_   = split;
        first_   = first;
        second_  = second;
        done_    = 0;
        const uint32_t generation = generation_ + 1;
        // The batch is in place before its number is: the cursor's store publishes it.
        InterlockedExchange64(&cursor_, static_cast<int64_t>(static_cast<uint64_t>(generation) << 32));
        InterlockedExchange(reinterpret_cast<volatile long*>(&generation_), static_cast<long>(generation));
        if (workers_) WakeByAddressAll(const_cast<uint32_t*>(&generation_));
        Work(generation);
        for (uint32_t spin = 0; static_cast<uint32_t>(done_) < count; ++spin)
        {
            if (spin < kSpins) __asm__ volatile("pause");
            else SwitchToThread();
        }
        // No job is left to claim: a late worker finds the cursor at the end, or a later batch's.
        MemoryBarrier();
    }

    // --- The scene's animation --------------------------------------------------------------------

    namespace animate
    {
        namespace
        {
            bool     g_read    = false;
            uint32_t g_threads = 0;  ///< in use, the main thread's included; 0: the client's own two
            bool     g_failed  = false;
            Pool     g_pool;

            struct Taken
            {
                void* cache = nullptr;
                void* scene = nullptr;
                void* head  = nullptr;
            };
            Taken g_taken;

            std::vector<void*> g_list;
            std::vector<void*> g_jobs;
            Counters           g_totals;

            uint32_t FromSettings()
            {
                const unsigned wanted = session::CurrentSettings().animate_threads;
                if (wanted != settings::kAnimateAuto) return wanted;
                SYSTEM_INFO info{};
                GetSystemInfo(&info);
                const uint32_t processors = info.dwNumberOfProcessors ? info.dwNumberOfProcessors : 1;
                return processors < 2 ? 0 : processors < 8 ? processors : 8;
            }

            void AnimateOne(void* scene, uint32_t index) { binding::m2_animate_model(g_jobs[index], scene); }
        }

        uint32_t Threads()
        {
            if (!g_read)
            {
                g_read    = true;
                g_threads = FromSettings();
            }
            return g_threads;
        }

        void SetThreadsForTests(uint32_t threads)
        {
            g_read    = true;
            g_threads = threads;
        }

        bool Begin(void* cache, void* function, void* scene)
        {
            if (!Threads() || g_failed || g_taken.cache || !cache || !scene) return false;
            if (!binding::m2_is_animate_thread(function)) return false;
            if (g_pool.Workers() + 1 < g_threads && !g_pool.Start(g_threads - 1))
            {
                g_failed = true;
                WLOG_WARN(kTag, "the models are animated on the client's own two threads: the runtime's could not start");
                return false;
            }

            uint32_t count = binding::m2_animate_list(scene, g_list.data(), static_cast<uint32_t>(g_list.size()));
            if (count > g_list.size())
            {
                g_list.resize(count + count / 2);
                count = binding::m2_animate_list(scene, g_list.data(), static_cast<uint32_t>(g_list.size()));
            }
            // The client's own split: the main thread the first, third... model, its worker the
            // second, fourth...; a model attached to another is its parent's to animate.
            g_jobs.clear();
            for (uint32_t i = 0; i < count; i += 2)
                if (!binding::m2_model_parent(g_list[i])) g_jobs.push_back(g_list[i]);
            const uint32_t split = static_cast<uint32_t>(g_jobs.size());
            for (uint32_t i = 1; i < count; i += 2)
                if (!binding::m2_model_parent(g_list[i])) g_jobs.push_back(g_list[i]);

            g_taken.cache = cache;
            g_taken.scene = scene;
            g_taken.head  = binding::m2_swap_animate_list(scene, nullptr);
            g_pool.Run(&AnimateOne, scene, static_cast<uint32_t>(g_jobs.size()), split, CaptureFloat(), g_pool.Fresh(),
                       g_threads - 1);
            ++g_totals.frames;
            g_totals.models += g_jobs.size();
            if (g_totals.frames == 1)
                WLOG_INFO(kTag, "animate_threads: the scene's models are animated on %u threads (the main thread and %u of the "
                                "runtime's); %u models this first time",
                          g_threads, g_threads - 1, static_cast<unsigned>(g_jobs.size()));
            return true;
        }

        bool Wait(void* cache)
        {
            if (!cache || cache != g_taken.cache) return false;
            binding::m2_swap_animate_list(g_taken.scene, g_taken.head);
            g_taken = Taken{};
            return true;
        }

        Counters Totals() { return g_totals; }
    }
}
