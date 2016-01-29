#include <pthread.h>
#include<iostream>
#include <stdio.h>
#include <unistd.h>
#include<list>
#include <semaphore.h>
#include<queue>
using namespace std;
// thread
static int flag = 0;
class CThread {
private:
	pthread_t m_thread;  //保持线程句柄
public:
	CThread(void* (*threadFuction)(void*),void* threadArgv);
	virtual ~CThread();

	void JoinThread();
};

CThread::CThread(void* (*threadFuction)(void*),void* threadArgv) {

	// 初始化线程属性
	pthread_attr_t threadAttr;
	pthread_attr_init(&threadAttr);

	pthread_create(&m_thread, &threadAttr, threadFuction, threadArgv);
}

CThread::~CThread() {

}


void CThread::JoinThread()
{
	// join
	pthread_join(m_thread, NULL);
}
//threadpool manager

class CThreadManager {
	friend void* ManageFuction(void*);
private:
	sem_t m_sem;	// 信号量
	pthread_mutex_t m_mutex; // 互斥锁
	queue<int> m_queWork; // 工作队列
	list<CThread*> m_lstThread; // 线程list

	int (*m_threadFuction)(int); //函数指针，指向main函数传过来的线程执行函数


public:
	CThreadManager(int (*threadFuction)(int), int nMaxThreadCnt);
	virtual ~CThreadManager();
	int WaitSem();
	int PostSem(); //wake up
	void PushWorkQue(int nWork);
	int PopWorkQue();
	int RunThreadFunction(int nWork);
	int start();
	int wait_all();
};
void* ManageFuction(void* argv)
{
	CThreadManager* pManager = (CThreadManager*)argv;

	// 进行无限循环（意味着线程是不销毁的，重复利用）
	while(true)
	{
		// 线程开启后，就在这里阻塞着，直到main函数设置了信号量
		pManager->WaitSem();
		// 从工作队列中取出要处理的数
		int nWork = pManager->PopWorkQue();
		pManager->RunThreadFunction(nWork);
	}

	return 0;
}
int CThreadManager::start(){
	int i = 0;
	cout << "all task" << flag<<endl;
	int tmp = flag;
	for(; i< tmp;++i){
		this->PostSem();
	}
	return 1;
}
int CThreadManager::wait_all(){
	while(flag){
		cout <<"Debug"<<flag<<endl;
	}
	return 1;
}
CThreadManager::CThreadManager(int (*threadFuction)(int), int nMaxThreadCnt) {
	
	sem_init(&m_sem, 0, 0);
	pthread_mutex_init(&m_mutex, NULL);
	m_threadFuction = threadFuction;

	for(int i=0; i<nMaxThreadCnt; i++)
	{
		CThread* pThread = new CThread(ManageFuction, this);
		m_lstThread.push_back(pThread);
	}
}

CThreadManager::~CThreadManager()
{
	sem_destroy(&m_sem);
	pthread_mutex_destroy(&m_mutex);
}

// 等待信号量
int CThreadManager::WaitSem()
{
	sem_wait(&m_sem);
}

// 设置信号量
int CThreadManager::PostSem()
{	
	 sem_post(&m_sem);
}	

// 往工作队列里放要处理的数
void CThreadManager::PushWorkQue(int nWork)
{
	m_queWork.push(nWork);
}

// 从工作队列中取出要处理的数
int CThreadManager::PopWorkQue()
{
	pthread_mutex_lock(&m_mutex);
	int nWork = m_queWork.front();
	m_queWork.pop();
	pthread_mutex_unlock(&m_mutex);
	return nWork;
}

// 执行main函数传过来的线程执行函数
int CThreadManager::RunThreadFunction(int nWork)
{
	(*m_threadFuction)(nWork);
	return 0;
}
int Count(int nWork)
{
	int nResult = nWork * nWork;
	printf("count result is %d\n",nResult);
	__sync_fetch_and_sub(&flag,1);
	return 0;
}

int main() {

	CThreadManager* pManager = new CThreadManager(Count, 10);
	int i = 0;
	for(;i < 30;++i){
	pManager->PushWorkQue(20);
	++flag;	
	}
	pManager->start();
	while(flag){}
	for(i = 0;i < 30;++i){
	pManager->PushWorkQue(30);
	++flag;
	}
	pManager->start();
	while(flag){}
	printf("hello world");
	return 0;
}


