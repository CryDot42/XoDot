/**************************************************************************/
/*  world_streaming.h                                                     */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/vector3.h"
#include "core/object/object.h"
#include "core/object/ref_counted.h"
#include "core/os/mutex.h"
#include "core/os/semaphore.h"
#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "core/templates/rid.h"
#include "core/templates/safe_refcount.h"

// A unit of streaming work (typically I/O and decompression).
// run() is executed on a streaming thread, finish() on the main thread once run() returned.
// Cancelled jobs never get finish() called.
class StreamingJob : public RefCounted {
	GDSOFTCLASS(StreamingJob, RefCounted);

	friend class WorldStreaming;

public:
	enum State {
		STATE_IDLE,
		STATE_QUEUED,
		STATE_RUNNING,
		STATE_DONE,
		STATE_FINISHED,
	};

private:
	SafeNumeric<uint32_t> state;
	SafeFlag cancelled;
	float priority = 0.0; // Protected by the WorldStreaming mutex.
	uint64_t order = 0;

protected:
	virtual void run() = 0;
	virtual void finish() {}

public:
	State get_state() const { return State(state.get()); }
	bool is_cancelled() const { return cancelled.is_set(); }
	bool is_pending() const {
		const uint32_t s = state.get();
		return s == STATE_QUEUED || s == STATE_RUNNING || s == STATE_DONE;
	}
};

// Shared infrastructure for everything that streams world content:
// - a prioritized job queue executed by dedicated streaming threads, with main thread completion,
// - streaming sources (players, cameras) that drive what should be resident,
// - named memory pools with budgets, exposed as performance monitors.
class WorldStreaming : public Object {
	GDCLASS(WorldStreaming, Object);

	static WorldStreaming *singleton;

public:
	struct Source {
		ObjectID id;
		RID scenario;
		Vector3 position;
		real_t range_scale = 1.0;
		real_t priority = 1.0;
	};

private:
	struct Pool {
		SafeNumeric<int64_t> usage;
		int64_t limit = 0;
	};

	// Jobs.
	mutable Mutex mutex;
	Semaphore semaphore;
	LocalVector<Thread *> threads;
	SafeFlag exit_threads;
	LocalVector<Ref<StreamingJob>> queue;
	LocalVector<Ref<StreamingJob>> completed;
	uint32_t running = 0;
	uint64_t order_counter = 0;
	SafeFlag process_queued;
	bool processing = false;

	static void _thread_func(void *p_userdata);
	void _run_job(const Ref<StreamingJob> &p_job);
	void _process_deferred();

	// Sources (main thread only).
	HashMap<ObjectID, Source> sources;

	// Memory pools.
	HashMap<StringName, Pool *> pools;
	Pool *_get_pool(const StringName &p_pool) const;
	Pool *_ensure_pool(const StringName &p_pool);

protected:
	static void _bind_methods();

public:
	static WorldStreaming *get_singleton() { return singleton; }

	// Jobs. Higher priorities run first.
	void submit(const Ref<StreamingJob> &p_job, float p_priority);
	void set_job_priority(const Ref<StreamingJob> &p_job, float p_priority);
	void cancel(const Ref<StreamingJob> &p_job);
	// Runs the job right away if it didn't start yet, otherwise waits for it. Calls finish().
	void wait(const Ref<StreamingJob> &p_job);
	// Calls finish() on the completed jobs. Called every frame, may be called more often.
	void process();
	int get_queued_job_count() const;
	int get_running_job_count() const;
	int get_thread_count() const { return threads.size(); }

	// Sources.
	void set_source(ObjectID p_id, RID p_scenario, const Vector3 &p_position, real_t p_range_scale = 1.0, real_t p_priority = 1.0);
	void remove_source(ObjectID p_id);
	LocalVector<Source> get_sources(RID p_scenario) const;
	int get_source_count() const { return sources.size(); }

	// Memory pools (thread-safe).
	void set_pool_limit(const StringName &p_pool, int64_t p_bytes);
	int64_t get_pool_limit(const StringName &p_pool) const;
	void add_pool_usage(const StringName &p_pool, int64_t p_bytes);
	int64_t get_pool_usage(const StringName &p_pool) const;
	bool is_pool_over_limit(const StringName &p_pool) const;
	PackedStringArray get_pool_names() const;

	void start_threads();
	void finish_threads();

	WorldStreaming();
	~WorldStreaming();
};
