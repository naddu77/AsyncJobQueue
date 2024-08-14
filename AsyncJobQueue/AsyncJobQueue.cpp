#include "AsyncJobQueue.h"

AsyncJobQueue<NoKey>::AsyncJobQueue(std::size_t number_of_threads)
	: thread_pool{ number_of_threads }
	, number_of_busy_threads{ std::size(thread_pool) }
{
	std::ranges::generate(thread_pool, [this] {
		return std::jthread{ std::bind_front(&AsyncJobQueue::JobDispatcherThread, this) };
	});
}

AsyncJobQueue<NoKey>::~AsyncJobQueue()
{
	for (auto& t : thread_pool)
	{
		t.request_stop();
	}

	job_condition_variable.notify_all();
}

void AsyncJobQueue<NoKey>::Join()
{
	job_condition_variable.notify_all();

	std::unique_lock lk{ mutex_for_condition_variable };

	join_condition_variable.wait(lk, [this] { return number_of_busy_threads == 0; });
}

void AsyncJobQueue<NoKey>::Cancel()
{
	decltype(job_queue) temp;

	std::lock_guard lk{ mutex_for_condition_variable };

	temp.swap(job_queue);
}

void AsyncJobQueue<NoKey>::JobDispatcherThread(std::stop_token stop_token)
{
	while (true)
	{	
		if (std::unique_lock lk{ mutex_for_condition_variable }; 
			std::empty(job_queue))
		{
			--number_of_busy_threads;

			if (number_of_busy_threads == 0)
			{
				join_condition_variable.notify_all();
			}

			job_condition_variable.wait(lk, [this, &stop_token] { return stop_token.stop_requested() || !std::empty(job_queue); });			

			if (stop_token.stop_requested() && std::empty(job_queue))
			{
				break;
			}

			++number_of_busy_threads;
		}
		else
		{
			auto job{ std::move(job_queue.front()) };

			job_queue.pop();

			lk.unlock();

			job.get();
		}
	}
}
