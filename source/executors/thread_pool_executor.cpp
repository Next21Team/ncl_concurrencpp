#include "concurrencpp/errors.h"
#include "concurrencpp/utils/list.h"
#include "concurrencpp/threads/thread.h"
#include "concurrencpp/executors/thread_pool_executor.h"

#include <mutex>
#include <semaphore>
#include <algorithm>

using concurrencpp::thread_pool_executor;
using concurrencpp::details::idle_worker_set;
using concurrencpp::details::thread_pool_worker;

namespace concurrencpp::details {
    namespace {
        struct thread_pool_per_thread_data {
            thread_pool_worker* this_worker;
            std::size_t this_thread_index;
            const std::size_t this_thread_hashed_id;

            static std::size_t calculate_hashed_id() noexcept {
                const auto this_thread_id = thread::get_current_virtual_id();
                const std::hash<std::size_t> hash;
                return hash(this_thread_id);
            }

            thread_pool_per_thread_data() noexcept :
                this_worker(nullptr), this_thread_index(static_cast<std::size_t>(-1)), this_thread_hashed_id(calculate_hashed_id()) {}
        };

        thread_local thread_pool_per_thread_data s_tl_thread_pool_data;
    }  // namespace

    class alignas(CRCPP_CACHE_LINE_ALIGNMENT) thread_pool_worker {

       private:
        list<task> m_private_queue;
        std::vector<std::size_t> m_idle_worker_list;
        std::atomic_bool m_atomic_abort;
        thread_pool_executor& m_parent_pool;
        const std::size_t m_index;
        const std::size_t m_pool_size;
        const std::chrono::milliseconds m_max_idle_time;
        const std::string m_worker_name;
        alignas(CRCPP_CACHE_LINE_ALIGNMENT) std::mutex m_lock;
        list<task> m_public_queue;
        std::binary_semaphore m_semaphore;
        bool m_idle;
        bool m_abort;
        std::atomic_bool m_task_found_or_abort;
        thread m_thread;

        void balance_work();

        bool wait_for_task(std::unique_lock<std::mutex>& lock);
        bool drain_queue_impl();
        bool drain_queue();

        void work_loop();

        void ensure_worker_active(bool first_enqueuer, std::unique_lock<std::mutex>& lock);

       public:
        thread_pool_worker(thread_pool_executor& parent_pool, std::size_t index, std::size_t pool_size, std::chrono::milliseconds max_idle_time);

        thread_pool_worker(thread_pool_worker&& rhs) noexcept;
        ~thread_pool_worker() noexcept;

        void enqueue_foreign(concurrencpp::task& task);
        void enqueue_foreign(task* head, task* tail, std::size_t count);

        void enqueue_local(concurrencpp::task& task);

        void shutdown();

        std::chrono::milliseconds max_worker_idle_time() const noexcept;

        bool appears_empty() const noexcept;
    };
}  // namespace concurrencpp::details

idle_worker_set::idle_worker_set(std::size_t size) : m_approx_size(0), m_idle_flags(std::make_unique<padded_flag[]>(size)), m_size(size) {}

void idle_worker_set::set_idle(std::size_t idle_thread) noexcept {
    const auto before = m_idle_flags[idle_thread].flag.exchange(status::idle, std::memory_order_relaxed);
    if (before == status::idle) {
        return;
    }

    m_approx_size.fetch_add(1, std::memory_order_relaxed);
}

void idle_worker_set::set_active(std::size_t idle_thread) noexcept {
    const auto before = m_idle_flags[idle_thread].flag.exchange(status::active, std::memory_order_relaxed);
    if (before == status::active) {
        return;
    }

    m_approx_size.fetch_sub(1, std::memory_order_relaxed);
}

bool idle_worker_set::try_acquire_flag(std::size_t index) noexcept {
    const auto worker_status = m_idle_flags[index].flag.load(std::memory_order_relaxed);
    if (worker_status == status::active) {
        return false;
    }

    const auto before = m_idle_flags[index].flag.exchange(status::active, std::memory_order_relaxed);
    const auto swapped = (before == status::idle);
    if (swapped) {
        m_approx_size.fetch_sub(1, std::memory_order_relaxed);
    }

    return swapped;
}

std::size_t idle_worker_set::find_idle_worker(std::size_t caller_index) noexcept {
    if (m_approx_size.load(std::memory_order_relaxed) <= 0) {
        return static_cast<std::size_t>(-1);
    }

    const auto starting_pos =
        (caller_index != static_cast<std::size_t>(-1)) ? caller_index : (s_tl_thread_pool_data.this_thread_hashed_id % m_size);

    for (std::size_t i = 0; i < m_size; i++) {
        const auto index = (starting_pos + i) % m_size;
        if (index == caller_index) {
            continue;
        }

        if (try_acquire_flag(index)) {
            return index;
        }
    }

    return static_cast<std::size_t>(-1);
}

void idle_worker_set::find_idle_workers(std::size_t caller_index, std::vector<std::size_t>& result_buffer, std::size_t max_count) noexcept {
    assert(result_buffer.capacity() >= max_count);

    const auto approx_size = m_approx_size.load(std::memory_order_relaxed);
    if (approx_size <= 0) {
        return;
    }

    assert(caller_index < m_size);
    assert(caller_index == s_tl_thread_pool_data.this_thread_index);

    std::size_t count = 0;
    const auto max_waiters = std::min(static_cast<std::size_t>(approx_size), max_count);

    for (std::size_t i = 0; (i < m_size) && (count < max_waiters); i++) {
        const auto index = (caller_index + i) % m_size;
        if (index == caller_index) {
            continue;
        }

        if (try_acquire_flag(index)) {
            result_buffer.emplace_back(index);
            ++count;
        }
    }
}

thread_pool_worker::thread_pool_worker(thread_pool_executor& parent_pool,
                                       std::size_t index,
                                       std::size_t pool_size,
                                       std::chrono::milliseconds max_idle_time) :
    m_atomic_abort(false),
    m_parent_pool(parent_pool), m_index(index), m_pool_size(pool_size), m_max_idle_time(max_idle_time),
    m_worker_name(details::make_executor_worker_name(parent_pool.name)), m_semaphore(0), m_idle(true), m_abort(false),
    m_task_found_or_abort(false) {
    m_idle_worker_list.reserve(pool_size);
}

thread_pool_worker::thread_pool_worker(thread_pool_worker&& rhs) noexcept :
    m_parent_pool(rhs.m_parent_pool), m_index(rhs.m_index), m_pool_size(rhs.m_pool_size), m_max_idle_time(rhs.m_max_idle_time),
    m_semaphore(0), m_idle(true), m_abort(true) {
    std::abort();  // shouldn't be called
}

thread_pool_worker::~thread_pool_worker() noexcept {
    assert(m_idle);
    assert(!m_thread.joinable());
}

void thread_pool_worker::balance_work() {
    const auto task_count = m_private_queue.size();
    if (task_count < 2) {  // no point in donating tasks
        return;
    }

    // we assume all threads but us are idle, we also save at least one task for ourselves
    const auto max_idle_worker_count = std::min(m_pool_size - 1, task_count - 1);
    if (max_idle_worker_count == 0) {
        return;  // a thread-pool with a single thread
    }

    m_parent_pool.find_idle_workers(m_index, m_idle_worker_list, max_idle_worker_count);
    const auto idle_count = m_idle_worker_list.size();
    if (idle_count == 0) {
        return;
    }

    assert(idle_count <= task_count);
    const auto total_worker_count = (idle_count + 1);  // count ourselves, otherwise we'll donate everything.
    const auto donation_count = task_count / total_worker_count;
    auto extra = task_count - donation_count * total_worker_count;

    for (const auto idle_worker_index : m_idle_worker_list) {
        assert(idle_worker_index != m_index);
        assert(idle_worker_index < m_pool_size);

        auto current_donation_count = donation_count;

        if (extra != 0) {
            current_donation_count++;
            extra--;
        }

        auto [head, tail] = m_private_queue.pop_front(current_donation_count);
        m_parent_pool.worker_at(idle_worker_index).enqueue_foreign(head, tail, current_donation_count);
    }

    assert(!m_private_queue.empty());
    m_idle_worker_list.clear();
}

bool thread_pool_worker::wait_for_task(std::unique_lock<std::mutex>& lock) {
    assert(lock.owns_lock());

    if (!m_public_queue.empty() || m_abort) {
        return true;
    }

    lock.unlock();

    m_parent_pool.mark_worker_idle(m_index);

    auto event_found = false;
    const auto deadline = std::chrono::steady_clock::now() + m_max_idle_time;

    while (true) {
        if (!m_semaphore.try_acquire_until(deadline)) {
            if (std::chrono::steady_clock::now() <= deadline) {
                continue;  // handle spurious wake-ups
            } else {
                break;
            }
        }

        if (!m_task_found_or_abort.load(std::memory_order_relaxed)) {
            continue;
        }

        lock.lock();
        if (m_public_queue.empty() && !m_abort) {
            lock.unlock();
            continue;
        }

        event_found = true;
        break;
    }

    if (!lock.owns_lock()) {
        lock.lock();
    }

    if (!event_found || m_abort) {
        m_idle = true;
        lock.unlock();
        return false;
    }

    assert(!m_public_queue.empty());
    m_parent_pool.mark_worker_active(m_index);
    return true;
}

bool thread_pool_worker::drain_queue_impl() {
    auto aborted = false;

    while (!m_private_queue.empty()) {
        balance_work();

        if (m_atomic_abort.load(std::memory_order_relaxed)) {
            aborted = true;
            break;
        }

        assert(!m_private_queue.empty());
        auto& task = m_private_queue.pop_back();
        task.resume();
    }

    if (aborted) {
        std::unique_lock<std::mutex> lock(m_lock);
        m_idle = true;
        return false;
    }

    return true;
}

bool thread_pool_worker::drain_queue() {
    std::unique_lock<std::mutex> lock(m_lock);
    if (!wait_for_task(lock)) {
        return false;
    }

    assert(lock.owns_lock());
    assert(!m_public_queue.empty() || m_abort);

    m_task_found_or_abort.store(false, std::memory_order_relaxed);

    if (m_abort) {
        m_idle = true;
        return false;
    }

    assert(m_private_queue.empty());
    m_private_queue = std::move(m_public_queue);
    lock.unlock();

    return drain_queue_impl();
}

void thread_pool_worker::work_loop() {
    s_tl_thread_pool_data.this_worker = this;
    s_tl_thread_pool_data.this_thread_index = m_index;

    try {
        while (true) {
            if (!drain_queue()) {
                return;
            }
        }
    } catch (const errors::runtime_shutdown&) {
        std::unique_lock<std::mutex> lock(m_lock);
        m_idle = true;
    }
}

void thread_pool_worker::ensure_worker_active(bool first_enqueuer, std::unique_lock<std::mutex>& lock) {
    assert(lock.owns_lock());

    if (!m_idle) {
        lock.unlock();

        if (first_enqueuer) {
            m_semaphore.release();
        }

        return;
    }

    auto stale_worker = std::move(m_thread);
    m_thread = thread(m_worker_name, [this] {
        work_loop();
    });

    m_idle = false;
    lock.unlock();

    if (stale_worker.joinable()) {
        stale_worker.join();
    }
}

void thread_pool_worker::enqueue_foreign(concurrencpp::task& task) {
    std::unique_lock<std::mutex> lock(m_lock);
    if (m_abort) {
        throw_runtime_shutdown_exception(m_parent_pool.name);
    }

    m_task_found_or_abort.store(true, std::memory_order_relaxed);

    const auto is_empty = m_public_queue.empty();
    m_public_queue.push_back(task);
    ensure_worker_active(is_empty, lock);
}

void thread_pool_worker::enqueue_foreign(task* head, task* tail, std::size_t count) {
    std::unique_lock<std::mutex> lock(m_lock);
    if (m_abort) {
        throw_runtime_shutdown_exception(m_parent_pool.name);
    }

    m_task_found_or_abort.store(true, std::memory_order_relaxed);

    const auto is_empty = m_public_queue.empty();
    m_public_queue.push_back(head, tail, count);
    ensure_worker_active(is_empty, lock);
}

void thread_pool_worker::enqueue_local(concurrencpp::task& task) {
    if (m_atomic_abort.load(std::memory_order_relaxed)) {
        throw_runtime_shutdown_exception(m_parent_pool.name);
    }

    m_private_queue.push_back(task);
}

void thread_pool_worker::shutdown() {
    assert(!m_atomic_abort.load(std::memory_order_relaxed));
    m_atomic_abort.store(true, std::memory_order_relaxed);

    {
        std::unique_lock<std::mutex> lock(m_lock);
        m_abort = true;
    }

    m_task_found_or_abort.store(true, std::memory_order_relaxed);  // make sure the store is finished before notifying the worker.

    m_semaphore.release();

    if (m_thread.joinable()) {
        m_thread.join();
    }

    decltype(m_public_queue) public_queue;
    decltype(m_private_queue) private_queue;

    {
        std::unique_lock<std::mutex> lock(m_lock);
        public_queue = std::move(m_public_queue);
        private_queue = std::move(m_private_queue);
    }

    while (!public_queue.empty()) {
        auto& task = public_queue.pop_front();
        task.interrupt();
    }

    while (!private_queue.empty()) {
        auto& task = private_queue.pop_front();
        task.interrupt();
    }
}

std::chrono::milliseconds thread_pool_worker::max_worker_idle_time() const noexcept {
    return m_max_idle_time;
}

bool thread_pool_worker::appears_empty() const noexcept {
    return m_private_queue.empty() && !m_task_found_or_abort.load(std::memory_order_relaxed);
}

thread_pool_executor::thread_pool_executor(std::string_view pool_name, std::size_t pool_size, std::chrono::milliseconds max_idle_time) :
    executor(pool_name), m_round_robin_cursor(0), m_idle_workers(pool_size), m_abort(false) {
    m_workers.reserve(pool_size);

    for (std::size_t i = 0; i < pool_size; i++) {
        m_workers.emplace_back(*this, i, pool_size, max_idle_time);
    }

    for (std::size_t i = 0; i < pool_size; i++) {
        m_idle_workers.set_idle(i);
    }
}

thread_pool_executor::~thread_pool_executor() noexcept {}

void thread_pool_executor::find_idle_workers(std::size_t caller_index, std::vector<std::size_t>& buffer, std::size_t max_count) noexcept {
    m_idle_workers.find_idle_workers(caller_index, buffer, max_count);
}

thread_pool_worker& thread_pool_executor::worker_at(std::size_t index) noexcept {
    assert(index <= m_workers.size());
    return m_workers[index];
}

void thread_pool_executor::mark_worker_idle(std::size_t index) noexcept {
    assert(index < m_workers.size());
    m_idle_workers.set_idle(index);
}

void thread_pool_executor::mark_worker_active(std::size_t index) noexcept {
    assert(index < m_workers.size());
    m_idle_workers.set_active(index);
}

void thread_pool_executor::enqueue(concurrencpp::task& task) {
    const auto this_worker = details::s_tl_thread_pool_data.this_worker;
    const auto this_worker_index = details::s_tl_thread_pool_data.this_thread_index;

    if (this_worker != nullptr && this_worker->appears_empty()) {
        return this_worker->enqueue_local(task);
    }

    const auto idle_worker_pos = m_idle_workers.find_idle_worker(this_worker_index);
    if (idle_worker_pos != static_cast<std::size_t>(-1)) {
        return m_workers[idle_worker_pos].enqueue_foreign(task);
    }

    if (this_worker != nullptr) {
        return this_worker->enqueue_local(task);
    }

    const auto next_worker = m_round_robin_cursor.fetch_add(1, std::memory_order_relaxed) % m_workers.size();
    m_workers[next_worker].enqueue_foreign(task);
}
int thread_pool_executor::max_concurrency_level() const noexcept {
    return static_cast<int>(m_workers.size());
}

bool thread_pool_executor::shutdown_requested() const {
    return m_abort.load(std::memory_order_relaxed);
}

void thread_pool_executor::shutdown() {
    const auto abort = m_abort.exchange(true, std::memory_order_relaxed);
    if (abort) {
        return;  // shutdown had been called before.
    }

    for (auto& worker : m_workers) {
        worker.shutdown();
    }
}

std::chrono::milliseconds thread_pool_executor::max_worker_idle_time() const noexcept {
    return m_workers[0].max_worker_idle_time();
}
