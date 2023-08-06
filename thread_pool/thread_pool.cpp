#include "thread_pool.h"


const int TASK_MAX_THRESHHOLD = INT_MAX; // 最大任务数量
const int THREAD_MAX_THRESHHOLD = 1024; // 最大线程数量
const int THREAD_MAX_IDLE_TIME = 60; // 单位：秒

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


Result ThreadPool::submitTask(std::shared_ptr<Task> sp)
{
	std::unique_lock<std::mutex> lock(taskQueMtx_);

	// 等待一秒，一秒以内如果任务队列依然是满的则返回失败
	if (!notFull_.wait_for(lock, std::chrono::seconds(1),
		[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) {
		std::cerr << "task queue is full!" << std::endl;

		return Result(sp, false);
	}

	// 添加任务到队列中
	taskQue_.emplace(sp);
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

	return Result(sp);
}

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

void ThreadPool::threadFunc(int threadId)
{
	auto lastTime = std::chrono::high_resolution_clock().now();

	for (;;) {
		std::shared_ptr<Task> task;
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
			task->exec(); //执行提交的任务
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

///////////Task基类方法
Task::Task()
	: result_(nullptr)
{
}

void Task::exec()
{
	if (result_ != nullptr) {
		result_->setAnyVal(run()); //多态调用
	}
}

void Task::setResultThis(Result* res)
{
	result_ = res;
}

////////////////////////  Result类方法实现
Result::Result(std::shared_ptr<Task> task, bool isValid)
	: isValid_(isValid)
	, task_(task)
{
	task_->setResultThis(this);
}

// 设置返回值
void Result::setAnyVal(Any any)
{
	this->any_ = std::move(any); //保存task的返回值
	sem_.post(); // 获取到任务返回值，增加信号量资源
}

Any Result::get() // 给用户调用 获取返回值
{
	if (!isValid_) {
		return "";
	}
	sem_.wait(); //等待信号，让线程中的任务执行完再获取返回值
	return std::move(any_);
}
