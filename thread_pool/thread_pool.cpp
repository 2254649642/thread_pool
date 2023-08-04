#include "thread_pool.h"

const int TASK_MAX_THRESHHOLD = 1024;


///////////线程池方法实现
ThreadPool::ThreadPool()
	: initThreadSize_(4)
	, taskSize_(0)
	, taskQueMaxThreshHold_(TASK_MAX_THRESHHOLD)
	, poolMode_(PoolMode::MODE_FIXED)
{
}

ThreadPool::~ThreadPool() {

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

	// Cached模式 
	//if (poolMode_ == PoolMode::MODE_CACHED
	//	&& )

	return Result(sp);
}

void ThreadPool::start(int initThreadSize)
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

			while (taskQue_.size() == 0) {
				if (!isPoolRunning_) {
					threads_.erase(threadId);

					std::cout << "thread_id " << std::this_thread::get_id() << "exit!" << std::endl;
					exitCond_.notify_all();
				}

				if (poolMode_ == MODE_CACHED) {

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
			task->exec(); //执行任务，把任务的返回值setVal方法给Result
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
size_t Thread::generateId_ = 0;

Thread::Thread(ThreadFunc func)
	: func_(func)
	, threadId_(generateId_++)
{
}

Thread::~Thread()
{
}

void Thread::start()
{
	// 创建一个执行线程去执行func_函数
	std::thread t(func_, threadId_);
	// 分离线程 脱离主线程
	t.detach();
}

int Thread::getId() const
{
	return threadId_;
}

///////////Task基类方法
Task::Task()
	: result_(nullptr)
{
}

void Task::exec()
{
	if (result_ != nullptr) {
		result_->setVal(run()); //多态调用
	}
}

void Task::setResult(Result* res)
{
	result_ = res;
}

////////////////////////  Result类方法实现
Result::Result(std::shared_ptr<Task> task, bool isValid)
	: isValid_(isValid)
	, task_(task)
{
	task_->setResult(this);
}

void Result::setVal(Any any)
{
	this->any_ = std::move(any); //保存task的返回值
	sem_.post(); // 获取到任务返回值，增加信号量资源
}

Any Result::get() // 给用户调用
{
	if (!isValid_) {
		return "";
	}
	sem_.wait(); //等待信号，让线程中的任务执行完再获取返回值
	return std::move(any_);
}
