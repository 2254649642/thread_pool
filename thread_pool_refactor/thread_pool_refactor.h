#include <memory>
#include <vector>
#include <iostream>
#include <functional>
#include <queue>
#include <mutex>
#include <unordered_map>
#include <future>


const int TASK_MAX_THRESHHOLD = INT_MAX; // 最大任务数量
const int THREAD_MAX_THRESHHOLD = 1024; // 最大线程数量
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒


enum PoolMode {
	MODE_FIXED,
	MODE_CACHED,
};

class Thread {
public:
	// 线程函数对象类型
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func);

	~Thread();

	//启动线程
	void start();

	//获取线程id
	int getId() const;
private:
	ThreadFunc func_;
	static size_t generateNo_; // 生成的线程编号
	size_t threadNo_; //保存线程编号
};


class ThreadPool {
public:
	ThreadPool();
	~ThreadPool();

	void setMode(PoolMode mode);

	//设置task任务队列上线阈值
	void setTaskQueMaxThreshHold(int threshhold);

	//设置线程池cached模式下线阈值
	void setTaskQueSizeThreshHold(int threshhold);

	//给线程池提交任务
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		using Rtype = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<Rtype()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

		std::future<Rtype> result = task->get_future();

		//获取锁
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		// 等待一秒，一秒以内如果任务队列依然是满的则返回失败
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) {

			std::cerr << "task queue is full!" << std::endl;
			auto task = std::make_shared<std::packaged_task<Rtype()>> ([]()->Rtype { return Rtype(); });

			(*task)();

			return task->get_future();
		}

		// 添加任务到队列中
		taskQue_.emplace([task]() {(*task)(); });
		taskSize_++;

		// 通知其他阻塞线程有任务可以执行了
		notEmpty_.notify_all();

		//CACHED模式 适合处理任务比较紧急 小而快的任务，根据当前任务数量和空闲线程数量决定是否创建新线程
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_) {

			std::cout << "create new thread..." << std::endl;

			// 创建新线程
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			size_t threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));

			//启动线程
			threads_[threadId]->start();

			//改变线程个数
			curThreadSize_++;
			idleThreadSize_++;
		}

		return result;
	}

	//开启线程池
	void start(int initThreadSize);

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//定义线程函数
	void threadFunc(int threadId);

	//检查pool运行状态
	bool checkRunnigState() const;

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; // 线程列表

	int initThreadSize_; //初始的线程数量
	int threadSizeThreshHold_; //线程数量上限阈值
	std::atomic_int curThreadSize_; //记录当前线程池里面线程的总数量
	std::atomic_int idleThreadSize_; // 记录空闲线程的数量

	using Task = std::function<void()>;
	std::queue<Task> taskQue_; //任务队列
	std::atomic_int taskSize_; //任务数量
	int taskQueMaxThreshHold_; //任务队列数量上限阈值

	std::mutex taskQueMtx_; //保证任务队列的线程安全
	std::condition_variable notFull_; //表示任务队列不空
	std::condition_variable notEmpty_; //表示任务队列不空
	std::condition_variable exitCond_; //等到线程资源全部回收

	PoolMode poolMode_; //当前线程池的工作模式
	std::atomic_bool isPoolRunning_; //表示当前线程池启动状态
};


///////////线程池方法实现
ThreadPool::ThreadPool()
	: initThreadSize_(0)
	, taskSize_(0)
	, idleThreadSize_(0)
	, isPoolRunning_(false)
	, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
	, threadSizeThreshHold_(THREAD_MAX_THRESHHOLD)
	, poolMode_(PoolMode::MODE_FIXED)
{
}

ThreadPool::~ThreadPool() {
	isPoolRunning_ = false;

	// 等待线程池所有线程执行完毕返回，线程可能处于2种状态 1: 阻塞 2: 正在执行任务中
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all();
	exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
}

// 设置线程池的工作模式
void ThreadPool::setMode(PoolMode mode)
{
	poolMode_ = mode;
}

// 设置task任务队列上线阈值
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
	taskQueMaxThreshHold_ = threshhold;
}

// 设置线程池cached模式下线程阈值
void ThreadPool::setTaskQueSizeThreshHold(int threshhold)
{
	if (poolMode_ == PoolMode::MODE_CACHED) {
		threadSizeThreshHold_ = threshhold;
	}
}

//线程池开始执行任务
void ThreadPool::start(int initThreadSize = 4) //默认4个线程执行任务
{
	// 记录初始线程个数
	initThreadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;

	// 设置线程池运行状态
	isPoolRunning_ = true;

	// 创建线程对象
	for (int i = 0; i < initThreadSize_; i++) {
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		//auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, std::placeholders::_1, this));

		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));

	}

	// 启动所有线程来工作
	for (int i = 0; i < initThreadSize_; i++)
	{
		threads_[i]->start(); // 启动一个线程
		idleThreadSize_++; // 记录空闲线程数量
	}
}

//线程入口函数
void ThreadPool::threadFunc(int threadId)
{
	auto lastTime = std::chrono::high_resolution_clock().now();

	for (;;) {
		Task task;
		{
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid " << std::this_thread::get_id()
				<< "尝试获取任务 " << std::endl;

			// 锁+双重判断
			while (taskQue_.size() == 0) {

				//线程池是否已经关闭
				if (!isPoolRunning_) {
					threads_.erase(threadId);

					std::cout << "thread_id " << std::this_thread::get_id() << "exit!" << std::endl;
					exitCond_.notify_all();
					return; // 线程函数结束，线程结束
				}

				// Cached模式 
				if (poolMode_ == MODE_CACHED) {

					//条件变量 判断当前线程是否空闲60s，决定是否回收
					if (std::cv_status::timeout == notEmpty_.wait_for(lock, std::chrono::seconds(1))) {
						auto now = std::chrono::high_resolution_clock().now();
						auto dur = std::chrono::duration_cast<std::chrono::seconds>(now - lastTime);

						if (dur.count() >= THREAD_MAX_IDLE_TIME
							&& curThreadSize_ > initThreadSize_) {

							threads_.erase(threadId);
							curThreadSize_--;
							idleThreadSize_--;

							std::cout << "thread_id " << std::this_thread::get_id() << "exit!" << std::endl;
							return;
						}
					}

				}
				else {
					notEmpty_.wait(lock);
				}
			}
			idleThreadSize_--;

			std::cout << "tid " << std::this_thread::get_id() << "获取任务成功 "
				<< std::endl;

			//从任务队列取一个任务出来执行
			task = taskQue_.front();
			taskQue_.pop();
			taskSize_--;

			// 如果还有剩余任务，通知其他线程快来执行任务
			if (taskQue_.size() > 0) {
				notEmpty_.notify_all();
			}

			// 取出一个任务，进行通知，通知可以继续生产任务
			notFull_.notify_all();
		} // 额外作用域 让锁自动析构

		if (task != nullptr) {
			task(); //执行提交的任务
		}

		idleThreadSize_++;
		lastTime = std::chrono::high_resolution_clock().now(); //更新线程执行时间

	}
}

bool ThreadPool::checkRunnigState() const
{
	return isPoolRunning_;
}

///////////////// 线程方法实现
size_t Thread::generateNo_ = 0;

Thread::Thread(ThreadFunc func)
	: func_(func)
	, threadNo_(generateNo_++)
{
}

Thread::~Thread()
{
}

void Thread::start()
{
	// 创建一个执行线程去执行func_函数
	std::thread t(func_, threadNo_);
	// 分离线程 脱离主线程
	t.detach();
}

int Thread::getId() const
{
	return threadNo_;
}

