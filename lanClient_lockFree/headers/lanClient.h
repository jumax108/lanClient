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

	// 필요한 메모리를 할당하고 서버를 시작합니다.
	bool Connect(const wchar_t* serverIP, unsigned short port, int sendBufferSize, int recvBufferSize,
		int createWorkerThreadNum, int runningWorkerThreadNum);
	// 모든 메모리를 정리하고 서버를 종료합니다.
	void Disconnect(); 

	// sessinoID 에 해당하는 세션에 데이터 전송합니다.
	bool sendPacket(CPacketPtr_Lan packet);
	 
	// 클라이언트가 접속을 완료한 상태에서 호출됩니다.
	virtual void onClientJoin(unsigned int ip, unsigned short port) = 0;
	// 클라이언트의 연결이 해제되면 호출됩니다.
	virtual void onClientLeave() = 0;

	// 클라이언트에게 데이터를 전송하면 호출됩니다.
	virtual void onRecv(CPacketPointer pakcet) = 0;
	// 클라이언트에게서 데이터를 전달받으면 호출됩니다.
	virtual void onSend(int sendSize) = 0;

	// 에러 상황에서 호출됩니다.
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

	// thread 정리용 event
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

		// ID의 하위 6바이트는 세션 메모리에 대한 재사용 횟수
		// 상위 2바이트는 세션 인덱스
		unsigned __int64 _sessionID; // 서버 가동 중에는 고유한 세션 ID

		CLockFreeQueue<CPacketPointer> _sendQueue;
		CRingBuffer _recvBuffer;
		
		OVERLAPPED _sendOverlapped;
		OVERLAPPED _recvOverlapped;

		CPacketPointer* _packets;

		SOCKET _sock;
	
		int _packetCnt;

		unsigned int _ip;
		unsigned short _port;

		// send를 1회로 제한하기 위한 플래그
		bool _isSent;
		
		// 총 16비트로 릴리즈 플래그 변화와 ioCnt가 0인지 동시에 체크하기 위함
		alignas(32) unsigned short _ioCnt;
		private: unsigned char _dummy;
		public: bool _released;

		bool _callDisconnect;

	};
};