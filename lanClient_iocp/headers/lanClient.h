#pragma once

#include <WinSock2.h>
#pragma comment(lib,"ws2_32")
#include <WS2tcpip.h>
#include <windows.h>
#include <stdexcept>
#include <thread>
#include <new>
#include <crtdbg.h>

///////////////////////////////////////////////////////////////////
// lib
#include "objectFreeListTLS/headers/objectFreeListTLS.h"
#include "dump/headers/dump.h"
#include "log/headers/log.h"
#include "protocolBuffer/headers/protocolBuffer.h"
#include "packetPointer/headers/packetPointer.h"
#include "ringBuffer/headers/ringBuffer.h"

#include "stack/headers/stack.h"
#include "queue/headers/queue.h"

#pragma comment(lib, "lib/dump/dump")
#pragma comment(lib, "lib/log/log")
#pragma comment(lib, "lib/protocolBuffer/protocolBuffer")
#pragma comment(lib, "lib/packetPointer/packetPointer")
#pragma comment(lib, "lib/ringBuffer/ringBuffer")
///////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////
// header
#include "common.h"
#include "packetPointer_LanServer.h"
///////////////////////////////////////////////////////////////////

class CLanClient{

public:

	CLanClient(int maxPacketNum, int workerThreadNum);
	~CLanClient();
	
	bool Connect(const wchar_t* ip, unsigned short port, bool onNagle);
	bool Disconnect();
	bool sendPacket(CPacketPtr_Lan);

	virtual void OnEnterJoinServer() = 0;
	virtual void OnLeaveServer() = 0;

	virtual void OnRecv(CPacketPtr_Lan) = 0;
	virtual void OnSend(int sendsize) = 0;

	virtual void OnError(int errorcode, const wchar_t*) = 0;

	inline int getSendTPS(){
		return _sendTPS;
	}
	inline int getRecvTPS(){
		return _recvTPS;
	}

protected:
	
	SOCKET _sock;

	CQueue<CPacketPtr_Lan> _sendQueue;
	CRingBuffer _recvBuffer;
		
	// send를 1회로 제한하기 위한 플래그
	bool _sendPosted;

	OVERLAPPED _sendOverlapped;
	OVERLAPPED _recvOverlapped;

	CPacketPtr_Lan* _packets;
	int _packetNum;
	int _packetCnt;

	int _workerThreadNum;
	HANDLE* _workerThread;

	HANDLE _tpsCalcThread;

	HANDLE _iocp;

	// free list에서 할당할 때 사용
	HANDLE _heap;

	CRITICAL_SECTION _lock;

	bool _disconnected;

	int _ioCnt;

	int _sendCnt;
	int _recvCnt;
	int _sendTPS;
	int _recvTPS;

	HANDLE _stopEvent;

	const wchar_t* _ip;
	unsigned short _port;

	void sendPost();
	void recvPost();
	void release();

	static unsigned __stdcall tpsCalcFunc(void* args);

	static unsigned __stdcall completionStatusFunc(void* args);

	static unsigned __stdcall connectFunc(void *args);

	void checkCompletePacket(unsigned __int64 sessionID, CRingBuffer* recvBuffer);
};
