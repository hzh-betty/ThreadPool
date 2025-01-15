#pragma once
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <functional>
#include <queue>
#include <future>
#include <chrono>
#include <memory>

// 默认配置常量
static const size_t DEFAULT_THREAD_NUM = std::thread::hardware_concurrency(); // 默认线程数量
static const size_t MAX_THREAD_LIMIT = 1000;                                  // 最大线程数量限制
static const size_t MAX_TASK_DEFAULT_NUM = 10000;                            // 默认任务队列最大长度
static const std::chrono::seconds MAX_IDLE_TIME = std::chrono::seconds(60);   // 线程最大空闲时间
static const std::chrono::seconds SUBMIT_TIME = std::chrono::seconds(1);      // 任务提交最大等待时间

// 类型定义
using Task = std::function<void()>; // 任务类型
using ThreadId = std::thread::id;   // 线程ID类型

// 线程池工作模式枚举
enum class PoolMode
{
    MODE_FIXED, // 固定线程数模式
    MODE_CACHED // 动态线程数模式
};

// 普通线程类
class NormalThread
{
public:
    NormalThread() = default;
    virtual ~NormalThread() = default;

    NormalThread(NormalThread &&) = default;            // 支持移动构造
    NormalThread &operator=(NormalThread &&) = default; // 支持移动赋值
    NormalThread(NormalThread &) = delete;              // 禁止拷贝构造
    NormalThread &operator=(NormalThread &) = delete;   // 禁止拷贝赋值

    // 启动线程并执行任务
    void start(Task task)
    {
        std::unique_ptr<std::thread> pThread(new std::thread(task));
        _thread = std::move(pThread);
    }

    // 获取线程ID
    ThreadId getId() const { return _thread->get_id(); }

    // 等待线程结束
    void join()
    {
        if (_thread->joinable())
        {
            _thread->join();
        }
    }

protected:
    std::unique_ptr<std::thread> _thread; // 线程对象
};

// 执行任务线程类
class ExcuteThread : public NormalThread
{
public:
    bool _isIdle;     // 标记线程是否空闲
    bool _shouldExit; // 标记线程是否应退出

    ExcuteThread() : _isIdle(false), _shouldExit(false) {}
    virtual ~ExcuteThread() = default;
};

// 监视线程类
class MonitorThread : public NormalThread
{
public:
    MonitorThread() = default;
    virtual ~MonitorThread() = default;
};

// 线程池配置类
class PoolConfig
{
public:
    PoolConfig()
        : _quit(false), _curThreadNums(0),
          _idleThreadNums(0), _maxThreadNums(0),
          _taskMaxSize(MAX_TASK_DEFAULT_NUM), _poolMode(PoolMode::MODE_FIXED) {}

    virtual ~PoolConfig() = default;

    // 检查线程池是否处于运行状态
    bool checkRunningState() const { return !_quit; }

    // 获取最大线程数
    size_t getMaxCount() const { return _maxThreadNums.load(); }

    // 获取活动线程数
    size_t getActiveCount() const { return _curThreadNums.load(); }

    // 获取任务队列的最大任务数
    size_t getTaskMaxCount() const { return _taskMaxSize.load(); }

    // 获取空闲线程数
    size_t getIdleCount() const { return _idleThreadNums.load(); }

    // 获取线程池模式
    PoolMode getPoolMode() const { return _poolMode; }

    // 设置任务队列最大任务数
    void setTaskMaxCount(size_t newSize) { _taskMaxSize = newSize; }

protected:
    std::atomic<bool> _quit;             // 是否退出标志
    std::atomic<size_t> _curThreadNums;  // 当前线程数
    std::atomic<size_t> _idleThreadNums; // 空闲线程数
    std::atomic<size_t> _maxThreadNums;  // 最大线程数
    std::atomic<size_t> _taskMaxSize;    // 任务队列最大长度
    PoolMode _poolMode;                  // 线程池模式
};

class ThreadPool : public PoolConfig
{
public:
    // 单例模式，禁止复制构造和赋值操作
    ThreadPool(const ThreadPool &) = delete;
    ThreadPool &operator=(const ThreadPool &) = delete;
    ThreadPool(const ThreadPool &&) = delete;
    ThreadPool &operator=(const ThreadPool &&) = delete;

    // 获取线程池单例
    static ThreadPool *getInstance()
    {
        // once_flag与call_once保证单例只执行一次
        static std::once_flag flag;
        std::call_once(flag, []()
                       { _instance.reset(new ThreadPool()); });
        return _instance.get();
    }

    // 启动线程池
    void start(size_t maxThreadNums = DEFAULT_THREAD_NUM, PoolMode mode = PoolMode::MODE_FIXED)
    {
        // 检查数据是否合法
        checkDataRange(maxThreadNums);

        _maxThreadNums = maxThreadNums;
        _poolMode = mode;

        // 启动初始线程
        for (size_t i = 0; i < maxThreadNums; i++)
        {
            launchExThread();
        }

        // 启动监视线程
        if (_poolMode == PoolMode::MODE_CACHED)
        {
            launchMoThread();
        }
    }

    // 提交任务到线程池
    template <typename Func, typename... Ts>
    auto submitTask(Func &&func, Ts &&...params)
        -> std::future<decltype(func(params...))>
    {
        // 1. 封装任务
        auto execute = std::bind(std::forward<Func>(func), std::forward<Ts>(params)...); // 创建绑定函数
        using ReturnType = decltype(func(params...));
        using PackagedTask = std::packaged_task<ReturnType()>;
        auto task = std::make_shared<PackagedTask>(execute); // 创建任务包装器
        auto result = task->get_future();                    // 获取任务的结果

        // 2. 加入任务队列，并且判断是否需要增加线程
        {
            std::unique_lock<std::mutex> lock(_mtx); // 锁住线程池

            // 等待超过1s，任务提交失败
            if (!_notFull.wait_for(lock, SUBMIT_TIME, [this]()
                                   { return _tasksQueue.size() < _taskMaxSize; }))
            {
                std::cerr << "task queue is full, submit task fail." << std::endl;
                auto task = std::make_shared<PackagedTask>([]()
                                                           { return ReturnType(); });
                (*task)();
                return task->get_future();
            }

            _tasksQueue.emplace([task]()
                                { (*task)(); }); // 将任务加入队列
            if (_idleThreadNums > 0)
            {
                _notEmpty.notify_all(); // 通知空闲线程处理任务
            }
            else if (_curThreadNums < _maxThreadNums && _poolMode == PoolMode::MODE_CACHED)
            {
                launchExThread(); // 启动新线程处理任务
            }
        }

        return result; // 返回任务的future对象
    }

    // 动态调整线程池大小
    void adjustPoolSize(size_t newMaxThreadNums)
    {
        checkDataRange(newMaxThreadNums);

        // 1. 增加线程池大小，启动新的线程
        std::lock_guard<std::mutex> lock(_mtx); // 锁住线程池

        if (_poolMode == PoolMode::MODE_FIXED)
        {
            std::cerr << "Mode is MODE_FIXED, can not adjustPoolSize !" << std::endl;
            return;
        }

        if (newMaxThreadNums > _maxThreadNums)
        {
            for (size_t i = _curThreadNums; i < newMaxThreadNums; i++)
            {
                launchExThread();
            }
        }

        // 2. 减少线程池大小，标记空闲线程为退出
        else if (newMaxThreadNums < _maxThreadNums)
        {
            // 不能减小到默认数量以下
            if (newMaxThreadNums < DEFAULT_THREAD_NUM)
            {
                std::cerr << "In cached mode, the minimum number of threads is " << DEFAULT_THREAD_NUM << std::endl;
                newMaxThreadNums = DEFAULT_THREAD_NUM;
            }

            size_t threadsToRemove = _curThreadNums - newMaxThreadNums;
            for (auto iter = _excuteThreads.begin(); iter != _excuteThreads.end() && threadsToRemove > 0;)
            {
                if (iter->second->_isIdle && threadsToRemove > 0 &&
                    iter->second->_shouldExit == false)
                {
                    iter->second->_shouldExit = true;
                    --threadsToRemove;
                }
                else
                {
                    ++iter;
                }
            }
        }
        _maxThreadNums = newMaxThreadNums; // 更新最大线程数
    }

    // 清理单例资源
    ~ThreadPool()
    {
        // 1. 设置标记位，通知所有线程退出
        _quit = true;
        _notEmpty.notify_all(); // 通知所有线程退出
        _timeOut.notify_all();  // 通知监视线程退出

        // 2. 回收监视线程
        if (_poolMode == PoolMode::MODE_CACHED)
            _monitorThread->join();

        // 3. 回收执行任务所有线程
        for (auto iter = _excuteThreads.begin(); iter != _excuteThreads.end(); ++iter)
        {
            // 回收每个线程
            iter->second->join();
        }
    }

private:
    // 构造函数，初始化线程池
    ThreadPool() = default;

    // 执行任务
    void excuteTask()
    {
        while (true)
        {
            Task task;
            ThreadId threadId;

            // 1. 从任务队列提取任务，判断任务是否需要放入超时回收队列
            {
                std::unique_lock<std::mutex> lock(_mtx);
                threadId = std::this_thread::get_id();

                if (_poolMode == PoolMode::MODE_CACHED)
                {
                    // 等待任务超时且任务队列为空
                    bool isTimeOut = _notEmpty.wait_for(lock, MAX_IDLE_TIME, [this]
                                                        { return _quit || !_tasksQueue.empty(); });

                    // 线程超时或退出标记
                    if ((!isTimeOut || _excuteThreads[threadId]->_shouldExit))
                    {
                        // 不能低于默认大小
                        if (_curThreadNums <= DEFAULT_THREAD_NUM)
                        {
                            _notEmpty.wait(lock, [this]
                                           { return _quit || !_tasksQueue.empty(); });
                        }
                        else
                        {
                            --_idleThreadNums;
                            --_curThreadNums;
                            _timeOutQueue.emplace(threadId); // 超时线程加入回收队列
                            _timeOut.notify_all();
                            return;
                        }
                    }
                }
                else
                {
                    _notEmpty.wait(lock, [this]
                                   { return _quit || !_tasksQueue.empty(); });
                }

                if (_quit && _tasksQueue.empty()) // 退出条件
                {
                    return;
                }

                task = std::move(_tasksQueue.front()); // 获取任务
                _tasksQueue.pop();
                _excuteThreads[threadId]->_isIdle = false; // 标记线程正在执行任务
                --_idleThreadNums;                         // 空闲线程数减少

                if (_tasksQueue.size() > 0)
                    _notEmpty.notify_all(); // 还有任务可执行继续通知

                _notFull.notify_all(); // 通知可继续生产任务
            }

            // 2. 执行任务
            task();

            // 3. 修改线程状态
            {
                std::lock_guard<std::mutex> lock(_mtx);
                _excuteThreads[threadId]->_isIdle = true; // 执行完任务后标记为空闲
                ++_idleThreadNums;                        // 空闲线程数增加
            }
        }
    }

    // 启动一个执行任务线程
    void launchExThread()
    {
        std::unique_ptr<ExcuteThread> excu(new ExcuteThread());
        excu->start(std::bind(&ThreadPool::excuteTask, this));
        ThreadId id = excu->getId();
        _excuteThreads[id] = std::move(excu);
        ++_curThreadNums;  // 活跃线程数增加
        ++_idleThreadNums; // 空闲线程数增加
    }

    // 启动一个监视线程
    void launchMoThread()
    {
        auto moni = std::unique_ptr<MonitorThread>(new MonitorThread());
        moni->start(std::bind(&ThreadPool::monitorIdleThreads, this));
        _monitorThread = std::move(moni);
    }

    // 监视空闲线程并回收超时线程，打印日志
    void monitorIdleThreads()
    {
        while (!_quit)
        {
            std::unique_lock<std::mutex> lock(_mtx);
            _timeOut.wait(lock, [this]()
                          { return !_timeOutQueue.empty() || _quit; });

            // 回收线程
            while (!_timeOutQueue.empty())
            {
                ThreadId threadId = _timeOutQueue.front();
                _timeOutQueue.pop();
                _excuteThreads[threadId]->join();
            }

            // 打印线程池状态
            std::cout << "当前线程池可容纳最大线程数为：" << _maxThreadNums << std::endl;
            std::cout << "当前线程池的线程数为：" << _curThreadNums << std::endl;
            std::cout << "当前线程池的空闲线程数为：" << _idleThreadNums << std::endl;
        }
    }

    void checkDataRange(size_t newMaxThreadNums)
    {
        // 1. 确保线程池大小不为0
        if (newMaxThreadNums == 0)
        {
            std::cerr << "Thread pool size cannot be zero." << std::endl;
            return;
        }

        // 2. 如果线程池大小超过上限，设置为上限
        if (newMaxThreadNums > MAX_THREAD_LIMIT)
        {
            newMaxThreadNums = DEFAULT_THREAD_NUM;
            std::cerr << "Warning: Thread pool size cannot exceed " << MAX_THREAD_LIMIT << ". It has been capped." << std::endl;
        }
    }

private:
    std::mutex _mtx;                                                            // 互斥锁
    std::queue<Task> _tasksQueue;                                               // 任务队列
    std::condition_variable _notEmpty;                                          // 任务可用条件变量
    std::condition_variable _notFull;                                           // 任务可生产条件变量
    std::condition_variable _timeOut;                                           // 超时回收条件变量
    std::unordered_map<ThreadId, std::unique_ptr<ExcuteThread>> _excuteThreads; // 存储执行线程信息
    std::unique_ptr<MonitorThread> _monitorThread;                              // 监视空闲线程
    std::queue<ThreadId> _timeOutQueue;                                         // 超时线程队列
    static std::unique_ptr<ThreadPool> _instance;                               // 线程池单例
};
std::unique_ptr<ThreadPool> ThreadPool::_instance = nullptr;
