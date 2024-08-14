#include "AsyncJobQueue.h"

#include <iostream>
#include <format>
#include <ranges>
#include <atomic>

using namespace std::literals;

int main()
{
    {
        AsyncJobQueue job_queue;

        job_queue.Add([] {
            std::cout << "Start\n";

            std::this_thread::sleep_for(1s);

            std::cout << "End\n";
            });
        job_queue.Add([] {
            std::cout << "Start2\n";

            std::this_thread::sleep_for(1s);

            std::cout << "End2\n";
            });
        job_queue.AddWithCallback(
            [](bool result) {
                std::cout << std::format("Result: {}\n", result);
            },
            [] {
                return true;
            }
        );

        job_queue.Join();
    }
    
    {
        AsyncJobQueue<std::string> job_queue;
        std::atomic_int actual1;
        std::atomic_int actual2;

        for (auto i : std::views::iota(0, 100))
        {
            job_queue.Add("1", [&actual1] {
                std::this_thread::sleep_for(10ms);
                ++actual1;
            });
            job_queue.Add("2", [&actual2] {
                std::this_thread::sleep_for(10ms);
                ++actual2;
            });
        }

        job_queue.Cancel("2");
        job_queue.Join();

        std::cout << std::format("Actual 1: {}\n", actual1.load());
        std::cout << std::format("Actual 2: {}\n", actual2.load());
    }

    std::cout << "main() end\n";
}
