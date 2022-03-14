#pragma once

#include <WinSock2.h>
#pragma comment(lib,"ws2_32")
#include <WS2tcpip.h>
#include <stdexcept>
#include <thread>
#include <new>
#include <windows.h>
#include <crtdbg.h>

#include "dump.h"
#include "log.h"
#include "ObjectFreeListTLS.h"
#include "serverError.h"
#include "stack.h"
#include "stringParser.h"
#include "ringBuffer.h"
#include "protocolBuffer.h"
#include "lockFreeQueue.h"
#include "lockFreeStack.h"
#include "packetPtr_LanClient.h"
#include "common.h"

class CLanClient{

public:

	CLanClient();

	bool Connect(const wchar_t* ip, unsigned short port, int maxPacketNum, int workerThreadNum, bool onNagle);
	bool Disconnect();
	bool sendPacket(CPacketPtrLan);

	virtual void OnEnterJoinServer() = 0;
	virtual void OnLeaveServer() = 0;

	virtual void OnRecv(CPacketPtr) = 0;
	virtual void OnSend(int sendsize) = 0;

	virtual void OnError(int errorcode, const wchar_t*) = 0;

protected:


	// ID�� ���� 6����Ʈ�� ���� �޸𸮿� ���� ���� Ƚ��
	// ���� 2����Ʈ�� ���� �ε���
	unsigned __int64 _sessionID; // ���� ���� �߿��� ������ ���� ID

	CLockFreeQueue<CPacketPtr> _sendQueue;
	CRingBuffer _recvBuffer;
		
	// send�� 1ȸ�� �����ϱ� ���� �÷���
	bool _sendPosted;

	OVERLAPPED _sendOverlapped;
	OVERLAPPED _recvOverlapped;

	CPacketPtr* _packets;
	int _packetNum;
	int _packetCnt;

	int _workerThreadNum;
	HANDLE* _workerThread;

	SOCKET _sock;

	HANDLE _iocp;

	// free list���� �Ҵ��� �� ���
	HANDLE _heap;

	const wchar_t* _ip;
	unsigned short _port;

	void init();

	void sendPost();
	void recvPost();

	static unsigned __stdcall completionStatusFunc(void* args);

	static unsigned __stdcall connectFunc(void *args);

	void checkCompletePacket(unsigned __int64 sessionID, CRingBuffer* recvBuffer);
};
