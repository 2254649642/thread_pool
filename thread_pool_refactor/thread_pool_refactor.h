#include <memory>
#include <vector>
#include <iostream>
#include <functional>
#include <queue>
#include <mutex>
#include <unordered_map>
#include <future>


const int TASK_MAX_THRESHHOLD = INT_MAX; // �����������
const int THREAD_MAX_THRESHHOLD = 1024; // ����߳�����
const int THREAD_MAX_IDLE_TIME = 60; // ��λ����


enum PoolMode {
	MODE_FIXED,
	MODE_CACHED,
};

class Thread {
public:
	// �̺߳�����������
	using ThreadFunc = std::function<void(int)>;

	Thread(ThreadFunc func);

	~Thread();

	//�����߳�
	void start();

	//��ȡ�߳�id
	int getId() const;
private:
	ThreadFunc func_;
	static size_t generateNo_; // ���ɵ��̱߳��
	size_t threadNo_; //�����̱߳��
};


class ThreadPool {
public:
	ThreadPool();
	~ThreadPool();

	void setMode(PoolMode mode);

	//����task�������������ֵ
	void setTaskQueMaxThreshHold(int threshhold);

	//�����̳߳�cachedģʽ������ֵ
	void setTaskQueSizeThreshHold(int threshhold);

	//���̳߳��ύ����
	template<typename Func, typename... Args>
	auto submitTask(Func&& func, Args&&... args) -> std::future<decltype(func(args...))>
	{
		using Rtype = decltype(func(args...));
		auto task = std::make_shared<std::packaged_task<Rtype()>>(
			std::bind(std::forward<Func>(func), std::forward<Args>(args)...));

		std::future<Rtype> result = task->get_future();

		//��ȡ��
		std::unique_lock<std::mutex> lock(taskQueMtx_);

		// �ȴ�һ�룬һ������������������Ȼ�������򷵻�ʧ��
		if (!notFull_.wait_for(lock, std::chrono::seconds(1),
			[&]()->bool { return taskQue_.size() < (size_t)taskQueMaxThreshHold_; })) {

			std::cerr << "task queue is full!" << std::endl;
			auto task = std::make_shared<std::packaged_task<Rtype()>> ([]()->Rtype { return Rtype(); });

			(*task)();

			return task->get_future();
		}

		// ������񵽶�����
		taskQue_.emplace([task]() {(*task)(); });
		taskSize_++;

		// ֪ͨ���������߳����������ִ����
		notEmpty_.notify_all();

		//CACHEDģʽ �ʺϴ�������ȽϽ��� С��������񣬸��ݵ�ǰ���������Ϳ����߳����������Ƿ񴴽����߳�
		if (poolMode_ == PoolMode::MODE_CACHED
			&& taskSize_ > idleThreadSize_
			&& curThreadSize_ < threadSizeThreshHold_) {

			std::cout << "create new thread..." << std::endl;

			// �������߳�
			auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
			size_t threadId = ptr->getId();
			threads_.emplace(threadId, std::move(ptr));

			//�����߳�
			threads_[threadId]->start();

			//�ı��̸߳���
			curThreadSize_++;
			idleThreadSize_++;
		}

		return result;
	}

	//�����̳߳�
	void start(int initThreadSize);

	ThreadPool(const ThreadPool&) = delete;
	ThreadPool& operator=(const ThreadPool&) = delete;

private:
	//�����̺߳���
	void threadFunc(int threadId);

	//���pool����״̬
	bool checkRunnigState() const;

private:
	std::unordered_map<int, std::unique_ptr<Thread>> threads_; // �߳��б�

	int initThreadSize_; //��ʼ���߳�����
	int threadSizeThreshHold_; //�߳�����������ֵ
	std::atomic_int curThreadSize_; //��¼��ǰ�̳߳������̵߳�������
	std::atomic_int idleThreadSize_; // ��¼�����̵߳�����

	using Task = std::function<void()>;
	std::queue<Task> taskQue_; //�������
	std::atomic_int taskSize_; //��������
	int taskQueMaxThreshHold_; //�����������������ֵ

	std::mutex taskQueMtx_; //��֤������е��̰߳�ȫ
	std::condition_variable notFull_; //��ʾ������в���
	std::condition_variable notEmpty_; //��ʾ������в���
	std::condition_variable exitCond_; //�ȵ��߳���Դȫ������

	PoolMode poolMode_; //��ǰ�̳߳صĹ���ģʽ
	std::atomic_bool isPoolRunning_; //��ʾ��ǰ�̳߳�����״̬
};


///////////�̳߳ط���ʵ��
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

	// �ȴ��̳߳������߳�ִ����Ϸ��أ��߳̿��ܴ���2��״̬ 1: ���� 2: ����ִ��������
	std::unique_lock<std::mutex> lock(taskQueMtx_);
	notEmpty_.notify_all();
	exitCond_.wait(lock, [&]()->bool { return threads_.size() == 0; });
}

// �����̳߳صĹ���ģʽ
void ThreadPool::setMode(PoolMode mode)
{
	poolMode_ = mode;
}

// ����task�������������ֵ
void ThreadPool::setTaskQueMaxThreshHold(int threshhold)
{
	taskQueMaxThreshHold_ = threshhold;
}

// �����̳߳�cachedģʽ���߳���ֵ
void ThreadPool::setTaskQueSizeThreshHold(int threshhold)
{
	if (poolMode_ == PoolMode::MODE_CACHED) {
		threadSizeThreshHold_ = threshhold;
	}
}

//�̳߳ؿ�ʼִ������
void ThreadPool::start(int initThreadSize = 4) //Ĭ��4���߳�ִ������
{
	// ��¼��ʼ�̸߳���
	initThreadSize_ = initThreadSize;
	curThreadSize_ = initThreadSize;

	// �����̳߳�����״̬
	isPoolRunning_ = true;

	// �����̶߳���
	for (int i = 0; i < initThreadSize_; i++) {
		auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, this, std::placeholders::_1));
		//auto ptr = std::make_unique<Thread>(std::bind(&ThreadPool::threadFunc, std::placeholders::_1, this));

		int threadId = ptr->getId();
		threads_.emplace(threadId, std::move(ptr));

	}

	// ���������߳�������
	for (int i = 0; i < initThreadSize_; i++)
	{
		threads_[i]->start(); // ����һ���߳�
		idleThreadSize_++; // ��¼�����߳�����
	}
}

//�߳���ں���
void ThreadPool::threadFunc(int threadId)
{
	auto lastTime = std::chrono::high_resolution_clock().now();

	for (;;) {
		Task task;
		{
			std::unique_lock<std::mutex> lock(taskQueMtx_);

			std::cout << "tid " << std::this_thread::get_id()
				<< "���Ի�ȡ���� " << std::endl;

			// ��+˫���ж�
			while (taskQue_.size() == 0) {

				//�̳߳��Ƿ��Ѿ��ر�
				if (!isPoolRunning_) {
					threads_.erase(threadId);

					std::cout << "thread_id " << std::this_thread::get_id() << "exit!" << std::endl;
					exitCond_.notify_all();
					return; // �̺߳����������߳̽���
				}

				// Cachedģʽ 
				if (poolMode_ == MODE_CACHED) {

					//�������� �жϵ�ǰ�߳��Ƿ����60s�������Ƿ����
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

			std::cout << "tid " << std::this_thread::get_id() << "��ȡ����ɹ� "
				<< std::endl;

			//���������ȡһ���������ִ��
			task = taskQue_.front();
			taskQue_.pop();
			taskSize_--;

			// �������ʣ������֪ͨ�����߳̿���ִ������
			if (taskQue_.size() > 0) {
				notEmpty_.notify_all();
			}

			// ȡ��һ�����񣬽���֪ͨ��֪ͨ���Լ�����������
			notFull_.notify_all();
		} // ���������� �����Զ�����

		if (task != nullptr) {
			task(); //ִ���ύ������
		}

		idleThreadSize_++;
		lastTime = std::chrono::high_resolution_clock().now(); //�����߳�ִ��ʱ��

	}
}

bool ThreadPool::checkRunnigState() const
{
	return isPoolRunning_;
}

///////////////// �̷߳���ʵ��
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
	// ����һ��ִ���߳�ȥִ��func_����
	std::thread t(func_, threadNo_);
	// �����߳� �������߳�
	t.detach();
}

int Thread::getId() const
{
	return threadNo_;
}

