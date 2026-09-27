/**************************************************************************/
/*  world_streaming.cpp                                                   */
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

#include "world_streaming.h"

#include "core/config/project_settings.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "main/performance.h"

WorldStreaming *WorldStreaming::singleton = nullptr;

/* Jobs */

void WorldStreaming::_thread_func(void *p_userdata) {
	WorldStreaming *ws = static_cast<WorldStreaming *>(p_userdata);
	Thread::set_name("WorldStreaming");
	while (true) {
		ws->semaphore.wait();
		if (ws->exit_threads.is_set()) {
			break;
		}

		Ref<StreamingJob> job;
		{
			MutexLock lock(ws->mutex);
			int best = -1;
			for (uint32_t i = 0; i < ws->queue.size(); i++) {
				const StreamingJob *candidate = ws->queue[i].ptr();
				if (best < 0 || candidate->priority > ws->queue[best]->priority || (candidate->priority == ws->queue[best]->priority && candidate->order < ws->queue[best]->order)) {
					best = i;
				}
			}
			if (best < 0) {
				continue; // The job was cancelled or executed on the main thread.
			}
			job = ws->queue[best];
			ws->queue.remove_at_unordered(best);
			job->state.set(StreamingJob::STATE_RUNNING);
			ws->running++;
		}

		ws->_run_job(job);
	}
}

void WorldStreaming::_run_job(const Ref<StreamingJob> &p_job) {
	if (!p_job->is_cancelled()) {
		p_job->run();
	}
	{
		MutexLock lock(mutex);
		running--;
		p_job->state.set(StreamingJob::STATE_DONE);
		completed.push_back(p_job);
	}
	// Complete on the main thread even when nothing is being drawn (e.g. idle editor or headless).
	if (!Thread::is_main_thread() && !process_queued.is_set()) {
		process_queued.set();
		callable_mp(this, &WorldStreaming::_process_deferred).call_deferred();
	}
}

void WorldStreaming::_process_deferred() {
	process_queued.clear();
	process();
}

void WorldStreaming::submit(const Ref<StreamingJob> &p_job, float p_priority) {
	ERR_FAIL_COND(p_job.is_null());
	ERR_FAIL_COND_MSG(p_job->is_pending(), "The streaming job is already submitted.");
	p_job->cancelled.clear();
	if (threads.is_empty()) {
		// No streaming threads (e.g. threads are unsupported): run synchronously.
		p_job->state.set(StreamingJob::STATE_RUNNING);
		{
			MutexLock lock(mutex);
			running++;
		}
		_run_job(p_job);
		return;
	}
	{
		MutexLock lock(mutex);
		p_job->priority = p_priority;
		p_job->order = order_counter++;
		p_job->state.set(StreamingJob::STATE_QUEUED);
		queue.push_back(p_job);
	}
	semaphore.post();
}

void WorldStreaming::set_job_priority(const Ref<StreamingJob> &p_job, float p_priority) {
	ERR_FAIL_COND(p_job.is_null());
	MutexLock lock(mutex);
	p_job->priority = p_priority;
}

void WorldStreaming::cancel(const Ref<StreamingJob> &p_job) {
	ERR_FAIL_COND(p_job.is_null());
	p_job->cancelled.set();
	MutexLock lock(mutex);
	const int64_t index = queue.find(p_job);
	if (index >= 0) {
		queue.remove_at_unordered(index);
		p_job->state.set(StreamingJob::STATE_IDLE);
	}
}

void WorldStreaming::wait(const Ref<StreamingJob> &p_job) {
	ERR_FAIL_COND(p_job.is_null());
	bool run_here = false;
	{
		MutexLock lock(mutex);
		const int64_t index = queue.find(p_job);
		if (index >= 0) {
			queue.remove_at_unordered(index);
			p_job->state.set(StreamingJob::STATE_RUNNING);
			running++;
			run_here = true;
		}
	}
	if (run_here) {
		_run_job(p_job);
	} else {
		while (p_job->get_state() == StreamingJob::STATE_RUNNING) {
			OS::get_singleton()->delay_usec(50);
		}
	}
	if (p_job->get_state() == StreamingJob::STATE_DONE) {
		{
			MutexLock lock(mutex);
			completed.erase(p_job);
		}
		p_job->state.set(StreamingJob::STATE_FINISHED);
		if (!p_job->is_cancelled()) {
			p_job->finish();
		}
	}
}

void WorldStreaming::process() {
	ERR_FAIL_COND(!Thread::is_main_thread());
	if (processing) {
		return; // Avoid re-entrance from finish() callbacks.
	}
	processing = true;
	LocalVector<Ref<StreamingJob>> done;
	{
		MutexLock lock(mutex);
		done = LocalVector<Ref<StreamingJob>>(completed);
		completed.clear();
	}
	for (const Ref<StreamingJob> &job : done) {
		if (job->get_state() != StreamingJob::STATE_DONE) {
			continue; // Already finished by wait().
		}
		job->state.set(StreamingJob::STATE_FINISHED);
		if (!job->is_cancelled()) {
			job->finish();
		}
	}
	processing = false;
}

int WorldStreaming::get_queued_job_count() const {
	MutexLock lock(mutex);
	return queue.size();
}

int WorldStreaming::get_running_job_count() const {
	MutexLock lock(mutex);
	return running;
}

/* Sources */

void WorldStreaming::set_source(ObjectID p_id, RID p_scenario, const Vector3 &p_position, real_t p_range_scale, real_t p_priority) {
	Source source;
	source.id = p_id;
	source.scenario = p_scenario;
	source.position = p_position;
	source.range_scale = MAX(p_range_scale, real_t(0.0));
	source.priority = p_priority;
	sources[p_id] = source;
}

void WorldStreaming::remove_source(ObjectID p_id) {
	sources.erase(p_id);
}

LocalVector<WorldStreaming::Source> WorldStreaming::get_sources(RID p_scenario) const {
	LocalVector<Source> result;
	for (const KeyValue<ObjectID, Source> &kv : sources) {
		if (kv.value.scenario == p_scenario) {
			result.push_back(kv.value);
		}
	}
	return result;
}

/* Memory pools */

WorldStreaming::Pool *WorldStreaming::_get_pool(const StringName &p_pool) const {
	MutexLock lock(mutex);
	Pool *const *pool = pools.getptr(p_pool);
	return pool ? *pool : nullptr;
}

WorldStreaming::Pool *WorldStreaming::_ensure_pool(const StringName &p_pool) {
	bool created = false;
	Pool *pool = nullptr;
	{
		MutexLock lock(mutex);
		Pool **existing = pools.getptr(p_pool);
		if (existing) {
			pool = *existing;
		} else {
			pool = memnew(Pool);
			pools.insert(p_pool, pool);
			created = true;
		}
	}
	if (created && Thread::is_main_thread() && Performance::get_singleton()) {
		const StringName monitor = "Streaming/" + String(p_pool);
		if (!Performance::get_singleton()->has_custom_monitor(monitor)) {
			Vector<Variant> args;
			args.push_back(p_pool);
			Performance::get_singleton()->add_custom_monitor(monitor, callable_mp(this, &WorldStreaming::get_pool_usage), args, Performance::MONITOR_TYPE_MEMORY);
		}
	}
	return pool;
}

void WorldStreaming::set_pool_limit(const StringName &p_pool, int64_t p_bytes) {
	Pool *pool = _ensure_pool(p_pool);
	MutexLock lock(mutex);
	pool->limit = MAX(p_bytes, int64_t(0));
}

int64_t WorldStreaming::get_pool_limit(const StringName &p_pool) const {
	const Pool *pool = _get_pool(p_pool);
	if (!pool) {
		return 0;
	}
	MutexLock lock(mutex);
	return pool->limit;
}

void WorldStreaming::add_pool_usage(const StringName &p_pool, int64_t p_bytes) {
	Pool *pool = _get_pool(p_pool);
	if (!pool) {
		pool = _ensure_pool(p_pool);
	}
	pool->usage.add(p_bytes);
}

int64_t WorldStreaming::get_pool_usage(const StringName &p_pool) const {
	const Pool *pool = _get_pool(p_pool);
	return pool ? pool->usage.get() : 0;
}

bool WorldStreaming::is_pool_over_limit(const StringName &p_pool) const {
	const Pool *pool = _get_pool(p_pool);
	if (!pool) {
		return false;
	}
	MutexLock lock(mutex);
	return pool->limit > 0 && pool->usage.get() > pool->limit;
}

PackedStringArray WorldStreaming::get_pool_names() const {
	PackedStringArray names;
	MutexLock lock(mutex);
	for (const KeyValue<StringName, Pool *> &kv : pools) {
		names.push_back(kv.key);
	}
	return names;
}

/* Threads */

void WorldStreaming::start_threads() {
	if (!threads.is_empty()) {
		return;
	}
#ifdef THREADS_ENABLED
	const int count = CLAMP(int(GLOBAL_GET("streaming/threads/thread_count")), 1, 16);
	exit_threads.clear();
	for (int i = 0; i < count; i++) {
		Thread *thread = memnew(Thread);
		thread->start(&WorldStreaming::_thread_func, this);
		threads.push_back(thread);
	}
#endif
	if (Performance::get_singleton()) {
		const StringName queued = "Streaming/Queued Jobs";
		if (!Performance::get_singleton()->has_custom_monitor(queued)) {
			Performance::get_singleton()->add_custom_monitor(queued, callable_mp(this, &WorldStreaming::get_queued_job_count), Vector<Variant>());
		}
	}
}

void WorldStreaming::finish_threads() {
	if (threads.is_empty()) {
		return;
	}
	{
		MutexLock lock(mutex);
		for (const Ref<StreamingJob> &job : queue) {
			job->cancelled.set();
			job->state.set(StreamingJob::STATE_IDLE);
		}
		queue.clear();
	}
	exit_threads.set();
	semaphore.post(threads.size());
	for (Thread *thread : threads) {
		thread->wait_to_finish();
		memdelete(thread);
	}
	threads.clear();
	MutexLock lock(mutex);
	completed.clear();
}

void WorldStreaming::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_queued_job_count"), &WorldStreaming::get_queued_job_count);
	ClassDB::bind_method(D_METHOD("get_running_job_count"), &WorldStreaming::get_running_job_count);
	ClassDB::bind_method(D_METHOD("get_thread_count"), &WorldStreaming::get_thread_count);
	ClassDB::bind_method(D_METHOD("get_source_count"), &WorldStreaming::get_source_count);
	ClassDB::bind_method(D_METHOD("set_pool_limit", "pool", "bytes"), &WorldStreaming::set_pool_limit);
	ClassDB::bind_method(D_METHOD("get_pool_limit", "pool"), &WorldStreaming::get_pool_limit);
	ClassDB::bind_method(D_METHOD("get_pool_usage", "pool"), &WorldStreaming::get_pool_usage);
	ClassDB::bind_method(D_METHOD("is_pool_over_limit", "pool"), &WorldStreaming::is_pool_over_limit);
	ClassDB::bind_method(D_METHOD("get_pool_names"), &WorldStreaming::get_pool_names);
	ClassDB::bind_method(D_METHOD("process"), &WorldStreaming::process);
}

WorldStreaming::WorldStreaming() {
	singleton = this;
}

WorldStreaming::~WorldStreaming() {
	finish_threads();
	for (KeyValue<StringName, Pool *> &kv : pools) {
		memdelete(kv.value);
	}
	pools.clear();
	singleton = nullptr;
}
