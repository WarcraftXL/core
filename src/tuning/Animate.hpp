// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 WarcraftXL
//
// The scene's models animated on more threads than the client's two ([engine] animate_threads).
//
// CM2Scene::Animate, when the client animates on threads (M2UseThreads), hands every second model
// of its animate list to its one worker (CM2Cache::BeginThread with CM2Scene::AnimateThread),
// animates the others itself, and waits (CM2Cache::WaitThread). Each model is animated by
// CM2Model::AnimateMT, or AnimateMTSimple, which touch nothing but the model, its attachments and
// what the scene and the file hold read-only. The M2 seams' gates on BeginThread and WaitThread
// take that work over: every model of the list is animated exactly once, on a pool of threads the
// main thread is one of, with the floating point state of the thread the client would have run it
// on (the main thread's for the first, third... model, a new thread's for the others: on x86 the
// x87 precision and rounding decide the results), before BeginThread returns; the list is
// hidden from the client's own loop until WaitThread, which puts it back. Then the client goes on
// as it does after its worker has finished.
//
// Main thread for Begin and Wait; the jobs run on the pool's threads.

#pragma once

#include <cstdint>

namespace wxl::core::tuning
{
    /// A thread's floating point state: the x87 control word (x86) and MXCSR.
    struct FloatState
    {
        uint32_t control = 0; ///< x87 control word (x86 only; 0 on x64)
        uint32_t mxcsr   = 0;

        bool operator==(const FloatState& other) const { return control == other.control && mxcsr == other.mxcsr; }
    };

    FloatState CaptureFloat();
    void       LoadFloat(const FloatState& state);

    /**
     * A pool of worker threads that runs a batch of jobs with the calling thread. Each job runs
     * once, on one thread, with the floating point state its half of the batch names; the calling
     * thread's own state is back when Run returns. One batch at a time, from one thread.
     */
    class Pool
    {
    public:
        using Job = void (*)(void* context, uint32_t index);

        /// Grows the pool to @p workers threads (at most 15); false when one cannot start.
        bool Start(uint32_t workers);
        uint32_t Workers() const { return workers_; }
        /// The floating point state a new thread starts with, read on the first worker.
        const FloatState& Fresh() const { return fresh_; }

        /**
         * @brief Runs job(context, i) for every i < @p count: those below @p split with @p first
         *        loaded, the others with @p second, on the calling thread and at most @p helpers
         *        of the workers; returns once all have run.
         */
        void Run(Job job, void* context, uint32_t count, uint32_t split, const FloatState& first, const FloatState& second,
                 uint32_t helpers);

    private:
        static unsigned long __stdcall Main(void* self);
        void Work(uint32_t generation);

        uint32_t   workers_ = 0;
        FloatState fresh_;
        // The batch: written by the calling thread before the generation moves.
        Job        job_ = nullptr;
        void*      context_ = nullptr;
        uint32_t   count_ = 0;
        uint32_t   split_ = 0;
        volatile uint32_t helpers_ = 0; ///< the workers that take part in the batch: those of a lower index
        FloatState first_, second_;
        alignas(64) volatile uint32_t generation_ = 0;  ///< the batch's number, what the workers wait on
        alignas(64) volatile int64_t  cursor_ = 0;      ///< (generation << 32) | the next job
        alignas(64) volatile long     done_ = 0;        ///< jobs of the batch that have run
        alignas(64) volatile long     started_ = 0;     ///< workers that have read their first state
    };

    namespace animate
    {
        /// Reads [engine] animate_threads (once, on the main thread) and reports what it gives:
        /// 0 for the client's own two threads.
        uint32_t Threads();

        /// The BeginThread gate's body: true when the pool animated the scene's models.
        bool Begin(void* cache, void* function, void* scene);
        /// The WaitThread gate's body: true when Begin took the scene this cache animates.
        bool Wait(void* cache);

        struct Counters
        {
            uint64_t frames = 0; ///< scenes animated by the pool
            uint64_t models = 0; ///< models animated by it
        };
        Counters Totals();

        /// The harness: the threads in use, as the setting would give them (0 turns the pool off).
        void SetThreadsForTests(uint32_t threads);
    }
}
