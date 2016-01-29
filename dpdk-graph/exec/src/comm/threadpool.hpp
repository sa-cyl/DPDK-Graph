#include <pthread.h>
#include <stdio.h>
#include <list>
#include <queue>
#include <semaphore.h>
#include<iostream>
#include <rpc/rpc.h>
using namespace std;
typedef struct{
	int sockfd;
	int p;
	char * buf;
	CLIENT *cl;
	int olength;
	int type;
	int index;
	int * clientfds_addr;
	fd_set * allset;
	int j;
}m_work;
class CThread {
private:
	pthread_t m_thread;  //保持线程句柄
public:
	CThread(void* (*threadFuction)(void*),void* threadArgv);
	virtual ~CThread();
};
CThread::CThread(void* (*threadFuction)(void*),void* threadArgv) {
	pthread_attr_t threadAttr;
	pthread_attr_init(&threadAttr);
	pthread_create(&m_thread, &threadAttr, threadFuction, threadArgv);
}

CThread::~CThread() {
}

class CThreadManager {
	friend void* ManageFuction(void*);
private:
	sem_t m_sem;	// 信号量
	pthread_mutex_t m_mutex; // 互斥锁
	queue<m_work> m_queWork; // 工作队列
	list<CThread*> m_lstThread; // 线程list
	volatile int flag;
	int (*m_threadFuction_send)(int,CLIENT*,int,char* ,int,int); //send 函数指针，指向main函数传过来的线程执行函数
	int (*m_threadFuction_recv)(int,int*,int,fd_set*); // recv 

public:
	CThreadManager(int (*threadFuction1)(int,int*,int ,fd_set*),int (*threadFuction2)(int, CLIENT*, int ,char*,int,int), int nMaxThreadCnt);
	virtual ~CThreadManager();
	int WaitSem();
	int PostSem();
	int LockMutex();
	int UnlockMutex();
	void PushWorkQue(m_work nWork);
	m_work PopWorkQue();
	int RunThreadFunction(m_work nWork);
	void start();
	void start(int );
	void wait();
	void show_flag(){
		std::cout <<"show flag" << flag<<std::endl;	
	}
};
void CThreadManager::start(int val){
	for(int i= 0;i < val;++i){
		this->PostSem();
	}
}
void* ManageFuction(void* argv)
{
	CThreadManager* pManager = (CThreadManager*)argv;

	while(true)
	{
		pManager->WaitSem();
		pManager->LockMutex();
		m_work nWork = pManager->PopWorkQue();
		pManager->UnlockMutex();
		pManager->RunThreadFunction(nWork);
	}
	return 0;
}


CThreadManager::CThreadManager(int (*threadFuction1)(int,int*,int,fd_set*), int (*threadFuction2)(int,CLIENT*, int ,char*,int,int),int nMaxThreadCnt):flag(0) {

	sem_init(&m_sem, 0, 0);
	pthread_mutex_init(&m_mutex, NULL);
	m_threadFuction_recv = threadFuction1;
	m_threadFuction_send = threadFuction2;
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
	return sem_wait(&m_sem);
}
int CThreadManager::PostSem()
{
	return sem_post(&m_sem);
}
int CThreadManager::LockMutex()
{
	int n= pthread_mutex_lock(&m_mutex);
	return n;
}
int CThreadManager::UnlockMutex()
{
	return pthread_mutex_unlock(&m_mutex);
}

void CThreadManager::PushWorkQue(m_work nWork)
{
	pthread_mutex_lock(&m_mutex);
	m_queWork.push(nWork);
	this->PostSem();
	pthread_mutex_unlock(&m_mutex);
}

m_work CThreadManager::PopWorkQue()
{
	m_work nWork = m_queWork.front();
	m_queWork.pop();
	return nWork;
}
int CThreadManager::RunThreadFunction(m_work nWork)
{
	switch(nWork.type){
	case 1:
	//recv
		(*m_threadFuction_recv)(nWork.sockfd,nWork.clientfds_addr,nWork.index,nWork.allset);
		break;
	case 2:
	//send
		(*m_threadFuction_send)(nWork.sockfd,nWork.cl, nWork.p,nWork.buf,nWork.olength,nWork.j);
		break;
	}
	return 1;
}
void CThreadManager::start(){
	int tmp = flag;
	int i = 0;
	for(;i < tmp;++i){
		this->PostSem();
	}
	return;
}
void CThreadManager::wait(){
	while(this->flag){
	}
	return;
}


