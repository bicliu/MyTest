#include <stdio.h>
#include <WinSock2.h>
#include <process.h>
#include <windows.h>

#include "blog.h"
//#include <iostream>
//#include <map>
#include "sockpipe.h"

#pragma comment(lib,"WS2_32.lib") //��ʽ�����׽���
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#define PROTOCALNO 6960
#define HEADLEN 8

static inline void buffer_write32be(unsigned char* buf, unsigned int value) {
    buf[0] = value >> 24;
    buf[1] = value >> 16;
    buf[2] = value >> 8;
    buf[3] = value;
}

static inline unsigned int buffer_read32be(const unsigned char* buf) {
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

unsigned int __stdcall runpipe(void* pPara);
unsigned int __stdcall runserver(void* pPara);
unsigned int __stdcall runclient(void* pPara);

typedef struct sockpipe {
	HANDLE semOrder;
	HANDLE semCtrl;
    HANDLE evtShutdown;

	HANDLE server;
	HANDLE client;
    HANDLE spipe;

    SOCKET sockSend;

    int port;
    bool running;

    int (*filldata)(void*, int);

	int Run()
	{
        running = true;
        spipe = (HANDLE)_beginthreadex(NULL, 0, runpipe, this, 0, NULL);
        if (!spipe) {
            blog(LOG_ERROR, "server start failed!\n");
            running = false;
            return -1;
        }
        return 0;
	}

    int write(char * str, unsigned int len)
    {
        unsigned char head[HEADLEN];
        buffer_write32be(head, PROTOCALNO);
        buffer_write32be(head + 4, len);
        send(sockSend, (char *)head, HEADLEN, NULL);
        return send(sockSend, str, len, NULL);
    }

    void Stop()
    {
        WaitForSingleObject(semCtrl, INFINITE);
        running = false;
        ReleaseSemaphore(semCtrl, 1, NULL);

        SetEvent(evtShutdown);

        WaitForSingleObject(spipe, INFINITE); 
        CloseHandle(spipe);
    }

    bool IsRunning()
    {
        WaitForSingleObject(semCtrl, INFINITE);
        bool r = running;
        ReleaseSemaphore(semCtrl, 1, NULL);
        return r;
    }
} sockpipe_t;

unsigned int __stdcall runpipe(void* pPara)
{
    if (!pPara) {
        blog(LOG_ERROR, "runserver parameter is NULL");
        return 1;
    }
    sockpipe_t* pipe = (sockpipe_t*)pPara;

    int ret = 0;
    pipe->semOrder = CreateSemaphore(NULL, 0, 1, NULL);
    if (!pipe->semOrder) { blog(LOG_ERROR, "CreateSemaphore semOrder failed!\n"); ret = -1; goto CLEAN_END; }
    pipe->evtShutdown = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!pipe->evtShutdown) { blog(LOG_ERROR, "CreateEvent evtShutdown failed!\n"); ret = -2; goto CLEAN_ORD; }
    pipe->semCtrl = CreateSemaphore(NULL, 1, 1, NULL);
    if (!pipe->semCtrl) { blog(LOG_ERROR, "CreateSemaphore semCtrl failed!\n"); ret = -3; goto CLEAN_SHT; }

    pipe->server = (HANDLE)_beginthreadex(NULL, 0, runserver, pipe, 0, NULL);
    if (!pipe->server) { blog(LOG_ERROR, "server start failed!\n"); ret = -4; goto CLEAN_CTL; }
    WaitForSingleObject(pipe->semOrder, INFINITE);
    pipe->client = (HANDLE)_beginthreadex(NULL, 0, runclient, pipe, 0, NULL);
    if (!pipe->client) { blog(LOG_ERROR, "client start failed!\n"); ret = -5; goto CLEAN_SRV; }

    WaitForSingleObject(pipe->client, INFINITE);
    WaitForSingleObject(pipe->server, INFINITE);

    //�ر��׽���
    closesocket(pipe->sockSend);
    //��ֹʹ�� DLL
    WSACleanup();

    CloseHandle(pipe->client);
CLEAN_SRV:
    CloseHandle(pipe->server);
CLEAN_CTL:
    CloseHandle(pipe->semCtrl);
CLEAN_SHT:
    CloseHandle(pipe->evtShutdown);
CLEAN_ORD:
    CloseHandle(pipe->semOrder);
CLEAN_END:
    blog(LOG_INFO, "pipe thread exit\n");
    return ret;
}
unsigned int __stdcall runserver(void* pPara)
{
    if (!pPara) {
        blog(LOG_ERROR, "runserver parameter is NULL");
        return 1;
    }

    sockpipe_t* pipe = (sockpipe_t*)pPara;
    //��ʼ�� DLL
    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret < 0) {
        blog(LOG_ERROR, "WSAStartup failed!%d\n", ret);
        return 2;
    }
    //�����׽���
    SOCKET servSock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    //���׽���
    sockaddr_in sockAddr;
    memset(&sockAddr, 0, sizeof(sockAddr));  //ÿ���ֽڶ���0���
    sockAddr.sin_family = PF_INET;  //ʹ��IPv4��ַ
    sockAddr.sin_addr.s_addr = inet_addr("127.0.0.1");  //�����IP��ַ
    sockAddr.sin_port = htons(pipe->port);  //�˿�
    bind(servSock, (SOCKADDR*)&sockAddr, sizeof(SOCKADDR));
    //�������״̬
    listen(servSock, 20);

    //�ͷſͻ��˿����������ź�
    ReleaseSemaphore(pipe->semOrder, 1, NULL);

    //���տͻ�������
    SOCKADDR clntAddr;
    int nSize = sizeof(SOCKADDR);
    SOCKET clntSock = accept(servSock, (SOCKADDR*)&clntAddr, &nSize);

    //���շ��������ص�����
    char head[HEADLEN] = { 0 };
    char* szdata;
    while (true) {
        //head
        int rlen = recv(clntSock, head, HEADLEN, NULL);
        if (rlen < HEADLEN) {
            blog(LOG_ERROR, "recive invalid data len(%d):%s\n", rlen, head);
            break;
        }
        if (buffer_read32be((unsigned char*)head) != PROTOCALNO) break;
        unsigned int tlen = buffer_read32be((unsigned char *)(&head[4]));

        //�����������ݽ��д���
        szdata = new char[tlen];
        if (pipe->filldata && tlen > 0 && szdata) {
            rlen = recv(clntSock, szdata, tlen, NULL);
            if (rlen < tlen) break;
            pipe->filldata(szdata, rlen);
        }
        delete[] szdata;

        //�Ƿ�ر�
        if (!pipe->IsRunning()) break;
    }

    //�ر��׽���
    closesocket(clntSock);
    closesocket(servSock);
    //��ֹ DLL ��ʹ��
    //WSACleanup();
    blog(LOG_INFO, "server thread exit\n");
    return 0;
}
unsigned int __stdcall runclient(void* pPara)
{
    if (!pPara) {
        blog(LOG_ERROR, "runserver parameter is NULL");
        return 1;
    }

    sockpipe_t* pipe = (sockpipe_t*)pPara;
    //��ʼ��DLL
    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if( ret < 0) {
        blog(LOG_ERROR, "WSAStartup failed!%d\n", ret);
        return 2;
    }
    //�����׽���
    pipe->sockSend = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
    //���������������
    sockaddr_in sockAddr;
    memset(&sockAddr, 0, sizeof(sockAddr));  //ÿ���ֽڶ���0���
    sockAddr.sin_family = PF_INET;
    sockAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    sockAddr.sin_port = htons(pipe->port);
    connect(pipe->sockSend, (SOCKADDR*)&sockAddr, sizeof(SOCKADDR));

    //�ȴ�����
    WaitForSingleObject(pipe->evtShutdown, INFINITE);
    send(pipe->sockSend, "0", 1, NULL); //����server����

    blog(LOG_INFO, "client thread exit\n");
    return 0;
}

CellPipe::CellPipe(int port, int (*f)(void*, int))
{
    pipe = new sockpipe_t();
    sockpipe_t* ptr = (sockpipe_t*)pipe;
    ptr->port = port;
    ptr->filldata = f;
    running = false;
}

CellPipe::~CellPipe()
{
    sockpipe_t* ptr = (sockpipe_t*)pipe;
    Stop();
    delete(ptr);
    pipe = ptr = NULL;
}

int CellPipe::Start()
{
    sockpipe_t* ptr = (sockpipe_t*)pipe;
    int r = ptr->Run();
    if (r == 0) running = true;
    return r;
}

int CellPipe::Write(char* data, int len)
{
    sockpipe_t* ptr = (sockpipe_t*)pipe;
    return ptr->write(data, len);
}

void CellPipe::Stop()
{
    if (running) {
        sockpipe_t* ptr = (sockpipe_t*)pipe;
        running = false;
        ptr->Stop();
    }
}

/*std::map<int, sockpipe_t*> mapPipes;
sockpipe_t* NewPipe(int port)
{
    std::map<int, sockpipe_t*>::iterator l_it;;
    l_it = mapPipes.find(port);
    if (l_it != mapPipes.end())
        return NULL;
    mapPipes[port] = new sockpipe_t();
    return mapPipes[port];
}

sockpipe_t* FindPipe(int port)
{
    std::map<int, sockpipe_t*>::iterator l_it;;
    l_it = mapPipes.find(port);
    if (l_it == mapPipes.end())
        return NULL;
    return mapPipes[port];
}

extern "C" int pipe_start(int (*f)(void*, int), int port)
{
    sockpipe_t *sp = NewPipe(port);
    sp->port = port; sp->filldata = f;
    return sp->Run();
}
extern "C" int pipe_write(char * data, int len, int port)
{
    sockpipe_t* sp = FindPipe(port);
    return sp->write(data, len);
}
extern "C" void pipe_stop(int port)
{
    sockpipe_t* sp = FindPipe(port);
    sp->Stop();
}*/