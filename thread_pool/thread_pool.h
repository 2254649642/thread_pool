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

	//��Any�������data����ȡ����
	template<typename T>
	T cast_() {
		// ����ָ��ת������ָ�루�����ԣ�
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

	// ������
	template<typename T>
	class Derive : public Base {
	public:
		Derive(T data) : data_(data) {}
		T data_; // ����������������
	};

private:
	std::unique_ptr<Base> base_; //����ָ��
};

// �ź�����
// ����ʹ��mutex����ʵ�ִ���黥��ִ��
// ʹ��mutex + condition_variable����ʵ���߳�ͨ��(1��1��ͬʱһ��������������ͬʱһ������������)������-������ģ��
// ʹ���ź���(mutex + condition_variable + ��Դ����)����ʵ�֣��Զ�ԣ�ͬʱ���������������ͬʱ������������ѣ�������-������ģ�� ͨ��
class Semaphore {
public:
	Semaphore(int limit = 0)
		: resLimit_(limit)
	{}
	~Semaphore() = default;

	//��ȡһ���ź���
	void wait() {
		std::unique_lock<std::mutex> lock(mtx_);

		// �ȴ��ź�����Դ��û����������ǰ�߳�
		cond_.wait(lock, [&]()->bool {return resLimit_ > 0; });
		resLimit_--;
	}

	void post() {
		std::unique_lock<std::mutex> lock(mtx_);
		resLimit_++;

		cond_.notify_all(); // �ȴ�״̬ ֪ͨ��������wait�����ĵط����Լ���ִ��
	}
private:
	int resLimit_;
	std::mutex mtx_;
	std::condition_variable cond_;
};

class Task;

// task���񷵻�ֵ����Result
class Result {
public:
	Result(std::shared_ptr<Task> task, bool isValid = true);
	~Result() = default;

	Any get(); //���û����� ��ȡ����ֵ
	void setAnyVal(Any any); //����task�ķ���ֵ

private:
	Any any_;  // �洢����ķ���ֵ
	Semaphore sem_; // �߳�ͨ���ź�
	std::shared_ptr<Task> task_; // ָ���Ӧ��ȡ����ֵ���������
	std::atomic_bool isValid_; // ����ֵ�Ƿ���Ч
};

//�����������
class Task {
public:
	Task();
	~Task() = default;

	void exec();
	void setResultThis(Result* res);
	virtual Any run() = 0;

private:
	Result* result_; // Result������������� > Task��
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
	Result submitTask(std::shared_ptr<Task> sp);

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

	
	std::queue<std::shared_ptr<Task>> taskQue_; //�������
	std::atomic_int taskSize_; //��������
	int taskQueMaxThreshHold_; //�����������������ֵ
	std::atomic_int idleThreadSize_; // ��¼�����̵߳�����

	std::mutex taskQueMtx_; //��֤������е��̰߳�ȫ
	std::condition_variable notFull_; //��ʾ������в���
	std::condition_variable notEmpty_; //��ʾ������в���
	std::condition_variable exitCond_; //�ȵ��߳���Դȫ������

	PoolMode poolMode_; //��ǰ�̳߳صĹ���ģʽ
	std::atomic_bool isPoolRunning_; //��ʾ��ǰ�̳߳�����״̬
};