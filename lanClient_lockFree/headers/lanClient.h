#pragma once

#include <WinSock2.h>
#pragma comment(lib,"ws2_32")
#include <WS2tcpip.h>

#include <thread>
#include <new>
#include <windows.h>	

///////////////////////////////////////////////////////////////////
// lib
#include "objectFreeListTLS/headers/objectFreeListTLS.h"
#include "dump/headers/dump.h"
#include "log/headers/log.h"
#include "protocolBuffer/headers/protocolBuffer.h"
#include "packetPointer/headers/packetPointer.h"
#include "lockFreeStack/headers/lockFreeStack.h"
#include "lockFreeQueue/headers/lockFreeQueue.h"
#include "ringBuffer/headers/ringBuffer.h"

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

	struct stSession;

public:

	CLanClient();

	// �ʿ��� �޸𸮸� �Ҵ��ϰ� ������ �����մϴ�.
	bool Connect(const wchar_t* serverIP, unsigned short port, int sendBufferSize, int recvBufferSize,
		int createWorkerThreadNum, int runningWorkerThreadNum);
	// ��� �޸𸮸� �����ϰ� ������ �����մϴ�.
	void Disconnect(); 

	// sessinoID �� �ش��ϴ� ���ǿ� ������ �����մϴ�.
	bool sendPacket(CPacketPtr_Lan packet);
	 
	// Ŭ���̾�Ʈ�� ������ �Ϸ��� ���¿��� ȣ��˴ϴ�.
	virtual void onClientJoin(unsigned int ip, unsigned short port) = 0;
	// Ŭ���̾�Ʈ�� ������ �����Ǹ� ȣ��˴ϴ�.
	virtual void onClientLeave() = 0;

	// Ŭ���̾�Ʈ���� �����͸� �����ϸ� ȣ��˴ϴ�.
	virtual void onRecv(CPacketPointer pakcet) = 0;
	// Ŭ���̾�Ʈ���Լ� �����͸� ���޹����� ȣ��˴ϴ�.
	virtual void onSend(int sendSize) = 0;

	// ���� ��Ȳ���� ȣ��˴ϴ�.
	virtual void onError(int errorCode, const wchar_t* errorMsg) = 0;

private:

	static HANDLE _heap;

	stSession* _session;

	int _workerThreadNum;
	HANDLE* _workerThread;

	HANDLE _iocp;

	int _sendBufferSize;
	int _recvBufferSize;

	// logger
	CLog _log;

	// thread ������ event
	HANDLE _stopEvent;

	void sendPost();
	void recvPost();

	static unsigned __stdcall completionStatusFunc(void* args);
	static unsigned __stdcall acceptFunc(void* args);
	static unsigned __stdcall tpsCalcFunc(void* args);

	void checkCompletePacket(stSession* session, CRingBuffer* recvBuffer);

	void release();

	struct stSession{
		
		stSession(unsigned int sendQueueSize, unsigned int recvBufferSize);
		~stSession();

		// ID�� ���� 6����Ʈ�� ���� �޸𸮿� ���� ���� Ƚ��
		// ���� 2����Ʈ�� ���� �ε���
		unsigned __int64 _sessionID; // ���� ���� �߿��� ������ ���� ID

		CLockFreeQueue<CPacketPointer> _sendQueue;
		CRingBuffer _recvBuffer;
		
		OVERLAPPED _sendOverlapped;
		OVERLAPPED _recvOverlapped;

		CPacketPointer* _packets;

		SOCKET _sock;
	
		int _packetCnt;

		unsigned int _ip;
		unsigned short _port;

		// send�� 1ȸ�� �����ϱ� ���� �÷���
		bool _isSent;
		
		// �� 16��Ʈ�� ������ �÷��� ��ȭ�� ioCnt�� 0���� ���ÿ� üũ�ϱ� ����
		alignas(32) unsigned short _ioCnt;
		private: unsigned char _dummy;
		public: bool _released;

		bool _callDisconnect;

	};
};