#include <memory>
#include <vector>
#include <iostream>
#include <functional>
#include <queue>
#include <mutex>
#include <unordered_map>

enum PoolMode {
	MODE_FIXED,
	MODE_CACHED,
};

class Any {
public:
	Any() = default;
	~Any() = default;
	Any(const Any&) = delete;
	Any& operator=(const Any&) = delete;
	Any(Any&&) = default;
	Any& operator=(Any&&) = default;

	template<typename T>
	Any(T data) : base_(std::make_unique<Derive<T>>(data))
	{}

	//把Any对象里的data数据取出来
	template<typename T>
	T cast_() {
		// 基类指针转派生类指针（关联性）
		Derive<T>* pd = dynamic_cast<Derive<T>*>(base_.get());

		if (pd == nullptr) {
			throw "type is unmatch!";
		}
		return pd->data_;
	}

private:
	class Base {
	public:
		virtual ~Base() = default;
	};

	// 派生类
	template<typename T>
	class Derive : public Base {
	public:
		Derive(T data) : data_(data) {}
		T data_; // 保存其他任意类型
	};

private:
	std::unique_ptr<Base> base_; //基类指针
};

// 信号量类
// 单纯使用mutex可以实现代码块互斥执行
// 使用mutex + condition_variable可以实现线程通信(1对1，同时一个生产者生产，同时一个消费者消费)生产者-消费者模型
// 使用信号量(mutex + condition_variable + 资源计数)可以实现（对多对，同时多个生产者生产，同时多个消费者消费）生产者-消费者模型 通信
class Semaphore {
public:
	Semaphore(int limit = 0)
		: resLimit_(limit)
	{}
	~Semaphore() = default;

	//获取一个信号量
	void wait() {
		std::unique_lock<std::mutex> lock(mtx_);

		// 等待信号量资源，没有则阻塞当前线程
		cond_.wait(lock, [&]()->bool {return resLimit_ > 0; });
		resLimit_--;
	}

	void post() {
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;

		cond_.notify_all(); // 等待状态 通知条件变量wait阻塞的地方可以继续执行
	}
private:
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

class Task;

// task任务返回值类型Result
class Result {
public:
	Result(std::shared_ptr<Task> task, bool isValid = true);
	~Result() = default;

	Any get(); //给用户调用 获取返回值
	void setAnyVal(Any any); //保存task的返回值

private:
	Any any_;  // 存储任务的返回值
	Semaphore sem_; // 线程通信信号
	std::shared_ptr<Task> task_; // 指向对应获取返回值的任务对象
	std::atomic_bool isValid_; // 返回值是否有效
};

//抽象任务基类
class Task {
public:
	Task();
	~Task() = default;

	void exec();
	void setResultThis(Result* res);
	virtual Any run() = 0;

private:
	Result* result_; // Result对象的生命周期 > Task的
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
	Result submitTask(std::shared_ptr<Task> sp);

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

	
	std::queue<std::shared_ptr<Task>> taskQue_; //任务队列
	std::atomic_int taskSize_; //任务数量
	int taskQueMaxThreshHold_; //任务队列数量上限阈值
	std::atomic_int idleThreadSize_; // 记录空闲线程的数量

	std::mutex taskQueMtx_; //保证任务队列的线程安全
	std::condition_variable notFull_; //表示任务队列不空
	std::condition_variable notEmpty_; //表示任务队列不空
	std::condition_variable exitCond_; //等到线程资源全部回收

	PoolMode poolMode_; //当前线程池的工作模式
	std::atomic_bool isPoolRunning_; //表示当前线程池启动状态
};