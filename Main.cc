#include <thread>
#include <iostream>
#include <chrono>
#include <cassert>
#include "threadpool.hpp"

std::mutex Mutex;
void task(int taskId)
{
    {
        std::lock_guard<std::mutex> lock(Mutex);
        std::cout << "task-" << taskId << " begin!" << std::endl;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));

    {
        std::lock_guard<std::mutex> lock(Mutex);
        std::cout << "task-" << taskId << " end!" << std::endl;
    }
}
int main()
{
    ThreadPool::getInstance()->start(2, PoolMode::MODE_CACHED);
    for (int taskId = 0; taskId < 101; ++taskId)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ThreadPool::getInstance()->submitTask(task, taskId);
    }
    return 0;
}