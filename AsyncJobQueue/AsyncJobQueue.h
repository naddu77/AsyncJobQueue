#pragma once

#include <thread>
#include <mutex>
#include <functional>
#include <string>
#include <condition_variable>
#include <queue>
#include <future>
#include <map>
#include <vector>
#include <list>

struct NoKey
{
	// Nothing
};

template <typename Key = NoKey>
class AsyncJobQueue final
{
public:
	explicit AsyncJobQueue(std::size_t number_of_threads = std::thread::hardware_concurrency() * 2)
		: thread_pool{ number_of_threads }
	{
		std::ranges::generate(thread_pool, [this] {
			return std::jthread{ std::bind_front(&AsyncJobQueue::JobDispatcherThread, this) };
		});
	}

	~AsyncJobQueue()
	{
		for (auto& t : thread_pool)
		{
			t.request_stop();
		}

		job_condition_variable.notify_all();
	}

	template <typename Func, typename... Ts>
	requires std::is_same_v<std::invoke_result_t<Func, Ts...>, void>
	void Add(Key const& key, Func&& func, Ts&&... ts)
	{
		auto job{ [this] <typename... Xs>(Xs&&... xs) {
			std::invoke(std::forward<Xs>(xs)...);
		} };

		std::unique_lock lk{ mutex_for_condition_variable };

		job_list.emplace_back(key, std::async(std::launch::deferred, job, std::forward<Func>(func), std::forward<Ts>(ts)...));
		++pending_job_count_map[key];

		lk.unlock();

		job_condition_variable.notify_one();
	}

	template <typename Func, typename... Ts, std::invocable<std::invoke_result_t<Func, Ts...>> Callback>
	void AddWithCallback(Key const& key, Callback&& callback, Func&& func, Ts&&... ts)
	{
		if constexpr (std::is_same_v<std::invoke_result_t<Func, Ts...>, void>)
		{
			auto job{ [this] <typename... Xs>(Callback&& callback, Xs&&... xs) {
				std::invoke(std::forward<Xs>(xs)...);
				std::invoke(std::forward<Callback>(callback));
			} };

			std::lock_guard lk{ mutex_for_condition_variable };

			job_list.emplace_back(key, std::async(std::launch::deferred, job, std::forward<Callback>(callback), std::forward<Func>(func), std::forward<Ts>(ts)...));
			++pending_job_count_map[key];
		}
		else
		{
			auto job{ [this] <typename... Xs>(Callback&& callback, Xs&&... xs) {
				std::invoke(std::forward<Callback>(callback), std::invoke(std::forward<Xs>(xs)...));
			} };
			
			std::lock_guard lk{ mutex_for_condition_variable };

			job_list.emplace_back(key, std::async(std::launch::deferred, job, std::forward<Callback>(callback), std::forward<Func>(func), std::forward<Ts>(ts)...));
			++pending_job_count_map[key];
		}

		job_condition_variable.notify_one();
	}

	template <typename... Ts>
	void Join(Ts const&... ts)
	{
		std::unique_lock lk{ mutex_for_condition_variable };

		if constexpr (sizeof...(Ts) == 0)
		{		
			join_condition_variable.wait(lk, [this] {
				return std::empty(job_list) && std::empty(pending_job_count_map) && std::empty(in_progress_job_count_map);
			});
		}
		else
		{
			join_condition_variable.wait(lk, [this, &ts...] {
				return Ready(ts...);
			});
		}
	}

	template <typename... Ts>
	bool Ready(Ts const&... ts) const
	{
		return std::empty(job_list)
			&& (!pending_job_count_map.contains(ts) && ...)
			&& (!in_progress_job_count_map.contains(ts) && ...);
	}

	template <typename... Ts>
	void Cancel(Ts const&... ts)
	{
		std::unique_lock lk{ mutex_for_condition_variable };

		if constexpr (sizeof...(Ts) == 0)
		{
			job_list.clear();
			pending_job_count_map.clear();
		}
		else
		{
			std::erase_if(job_list, [&ts...](auto const& t) {
				auto const& key{ std::get<0>(t) };

				return ((key == ts) || ...);
			});
			(pending_job_count_map.erase(ts), ...);
		}
	}

private:
	std::mutex mutex_for_condition_variable;
	std::condition_variable job_condition_variable;
	std::condition_variable join_condition_variable;
	std::map<Key, std::size_t> in_progress_job_count_map;
	std::map<Key, std::size_t> pending_job_count_map;
	std::list<std::tuple<Key, std::future<void>>> job_list;
	std::vector<std::jthread> thread_pool;

	void JobDispatcherThread(std::stop_token stop_token)
	{
		while (true)
		{
			if (std::unique_lock lk{ mutex_for_condition_variable }; 
				std::empty(job_list))
			{
				join_condition_variable.notify_all();
				job_condition_variable.wait(lk, [this, &stop_token] { return stop_token.stop_requested() || !std::empty(job_list); });

				if (stop_token.stop_requested() && std::empty(job_list))
				{
					break;
				}
			}
			else
			{
				auto [key, job] { std::move(job_list.front()) };

				job_list.pop_front();

				auto& pending_job_count{ pending_job_count_map[key] };

				--pending_job_count;

				if (pending_job_count == 0)
				{
					pending_job_count_map.erase(key);
				}

				auto& in_progress_job_count{ in_progress_job_count_map[key] };

				++in_progress_job_count;

				lk.unlock();

				job.get();

				lk.lock();

				--in_progress_job_count;

				if (in_progress_job_count == 0)
				{
					in_progress_job_count_map.erase(key);

					if (!pending_job_count_map.contains(key))
					{
						join_condition_variable.notify_all();
					}
				}
			}
		}
	}
};

template <>
class AsyncJobQueue<NoKey> final
{
public:
	explicit AsyncJobQueue(std::size_t number_of_threads = std::thread::hardware_concurrency() * 2);
	~AsyncJobQueue();

	template <typename Func, typename... Ts>
	requires std::is_same_v<std::invoke_result_t<Func, Ts...>, void>
	void Add(Func&& func, Ts&&... ts)
	{
		auto job{ [this] <typename... Xs>(Xs&&... xs) {
			std::invoke(std::forward<Xs>(xs)...);
		} };

		std::unique_lock lk{ mutex_for_condition_variable };

		job_queue.push(std::async(std::launch::deferred, job, std::forward<Func>(func), std::forward<Ts>(ts)...));

		lk.unlock();

		job_condition_variable.notify_one();
	}

	template <typename Func, typename... Ts, std::invocable<std::invoke_result_t<Func, Ts...>> Callback>
	void AddWithCallback(Callback&& callback, Func&& func, Ts&&... ts)
	{
		if constexpr (std::is_same_v<std::invoke_result_t<Func, Ts...>, void>)
		{
			auto job{ [this] <typename... Xs>(Callback&& callback, Xs&&... xs) {
				std::invoke(std::forward<Xs>(xs)...);
				std::invoke(std::forward<Callback>(callback));
			} };

			std::lock_guard lk{ mutex_for_condition_variable };

			job_queue.push(std::async(std::launch::deferred, job, std::move(callback), std::forward<Func>(func), std::forward<Ts>(ts)...));
		}
		else
		{
			auto job{ [this] <typename... Xs>(Callback&& callback, Xs&&... xs) {
				std::invoke(std::forward<Callback>(callback), std::invoke(std::forward<Xs>(xs)...));
			} };
			
			std::lock_guard lk{ mutex_for_condition_variable };

			job_queue.push(std::async(std::launch::deferred, job, std::move(callback), std::forward<Func>(func), std::forward<Ts>(ts)...));
		}

		job_condition_variable.notify_one();
	}

	void Join();
	void Cancel();

private:
	std::mutex mutex_for_condition_variable;
	std::condition_variable job_condition_variable;
	std::condition_variable join_condition_variable;
	std::queue<std::future<void>> job_queue;
	std::vector<std::jthread> thread_pool;
	std::size_t number_of_busy_threads;

	void JobDispatcherThread(std::stop_token stop_token);
};
