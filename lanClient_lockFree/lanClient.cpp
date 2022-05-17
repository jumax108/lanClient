

#include "headers/lanClient.h"

HANDLE CLanClient::_heap = NULL;

CLanClient::CLanClient(){

	_workerThreadNum = 0;
	_workerThread = nullptr;

	_iocp = NULL;
	
	_log.setDirectory(L"log");
	_log.setPrintGroup(LOG_GROUP::LOG_ERROR | LOG_GROUP::LOG_SYSTEM);

	_stopEvent = CreateEvent(nullptr, true, false, L"stopEvent");

	_heap = HeapCreate(0, 0, 0);

	// wsa startup
	int startupResult;
	int startupError;
	{
		WSAData wsaData;
		startupResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
		if (startupResult != NULL) {
			startupError = WSAGetLastError();
			CDump::crash();
		}
	}

}

void CLanClient::Disconnect(){
	_session->_callDisconnect = true;
	closesocket(_session->_sock);
	_session->_sock = 0;

}

void CLanClient::release() {

	stSession* session = _session;

	if (InterlockedCompareExchange((UINT*)&session->_ioCnt, 1, 0) != 0) {
		return;
	}
		
	// send buffer 안에 있는 패킷들 제거
	CLockFreeQueue<CPacketPointer>* sendBuffer = &session->_sendQueue;
	int sendQueueSize = sendBuffer->getSize();

	for (int packetCnt = 0; packetCnt < sendQueueSize; ++packetCnt) {
		CPacketPointer* packet = &session->_packets[packetCnt];

		sendBuffer->pop(packet);

		packet->decRef();
		packet->~CPacketPointer();

	}

	CRingBuffer* recvBuffer = &session->_recvBuffer;
	recvBuffer->moveFront(recvBuffer->getUsedSize());

	closesocket(session->_sock);
	session->_sock = 0;

	this->onClientLeave();

}

bool CLanClient::sendPacket(CPacketPtr_Lan packet){
	
	stSession* session = _session;

	InterlockedIncrement16((SHORT*)&session->_ioCnt);
	if (session->_released == true || session->_callDisconnect == true) {
		if (InterlockedDecrement16((SHORT*)&session->_ioCnt) == 0) {
			release();
		}
		return false;
	}
	
	CLockFreeQueue<CPacketPointer>* sendQueue = &session->_sendQueue;
	packet.incRef();
	packet.setHeader();
	sendQueue->push(packet);
	
	sendPost();

	if (InterlockedDecrement16((SHORT*)&session->_ioCnt) == 0) {
		release();
	}

	return true;

}

unsigned CLanClient::completionStatusFunc(void *args){
	
	CLanClient* server = (CLanClient*)args;

	HANDLE iocp = server->_iocp;
	stSession* session = server->_session;

	for(;;){
		
		unsigned int transferred;
		OVERLAPPED* overlapped;
		unsigned __int64 sessionID;
		GetQueuedCompletionStatus(iocp, (LPDWORD)&transferred, (PULONG_PTR)&sessionID, &overlapped, INFINITE);
		
		InterlockedIncrement16((SHORT*)&session->_ioCnt);

		do {

			if(session->_released == true){
				break;
			}

			SOCKET sock = session->_sock;

			// send 완료 처리
			if(&session->_sendOverlapped == overlapped){
		
				int packetTotalSize = 0;

				int packetNum = session->_packetCnt;
				CPacketPointer* packets = session->_packets;
				CPacketPointer* packetIter = packets;
				CPacketPointer* packetEnd = packets + packetNum;
				for(; packetIter != packetEnd; ++packetIter){
					packetTotalSize += packetIter->getPacketPoolUsage();
					packetIter->decRef();
					packetIter->~CPacketPointer();
				}

				session->_packetCnt = 0;

				session->_isSent = false;
			
				server->onSend(packetTotalSize);

				CLockFreeQueue<CPacketPointer>* sendQueue = &session->_sendQueue;

				if (sendQueue->getSize() > 0) {
					server->sendPost();
				}
			}
			
			// recv 완료 처리
			else if(&session->_recvOverlapped == overlapped){

				CRingBuffer* recvBuffer = &session->_recvBuffer;

				recvBuffer->moveRear(transferred);

				// packet proc
				server->checkCompletePacket(session, recvBuffer);

				server->recvPost();
			
			}
			
		} while (false);

		if (InterlockedDecrement16((SHORT*)&session->_ioCnt) == 0) {
			server->release();
		}
	}

	return 0;
}

void CLanClient::recvPost(){
	
	stSession* session = _session;
	unsigned __int64 sessionID = session->_sessionID;

	if (session->_released == true) {
		return;
	}

	InterlockedIncrement16((SHORT*)&session->_ioCnt);

	/////////////////////////////////////////////////////////
	// recv buffer 정보를 wsa buf로 복사
	/////////////////////////////////////////////////////////
	WSABUF wsaBuf[2];
	int wsaCnt = 1;

	CRingBuffer* recvBuffer = &session->_recvBuffer;

	int rear = recvBuffer->rear();
	int front = recvBuffer->front();
	char* directPushPtr = recvBuffer->getDirectPush();
	int directFreeSize = recvBuffer->getDirectFreeSize();
	char* bufStartPtr = recvBuffer->getBufferStart();

	wsaBuf[0].buf = directPushPtr;
	wsaBuf[0].len = directFreeSize;

	if(front <= rear){
		wsaBuf[1].buf = bufStartPtr;
		wsaBuf[1].len = front;
		wsaCnt = 2;
	}
	/////////////////////////////////////////////////////////
	
	/////////////////////////////////////////////////////////
	// wsa recv
	/////////////////////////////////////////////////////////
	OVERLAPPED* overlapped = &session->_recvOverlapped;
	SOCKET sock = session->_sock;
	
	int recvResult;
	int recvError;

	unsigned int flag = 0;
	recvResult = WSARecv(sock, wsaBuf, wsaCnt, nullptr, (LPDWORD)&flag, overlapped, nullptr);
	if(recvResult == SOCKET_ERROR){
		recvError = WSAGetLastError();
		if(recvError != WSA_IO_PENDING){

			Disconnect();
			if (InterlockedDecrement16((SHORT*)&session->_ioCnt) == 0) {
				release();
			}

			if(recvError != 10054){
				_log(L"recv.txt", LOG_GROUP::LOG_DEBUG, L"session: 0x%I64x, sock: %I64d, wsaCnt: %d, wsaBuf[0]: 0x%I64x, wsaBuf[1]: 0x%I64x\n", sessionID, sock, wsaCnt, wsaBuf[0], wsaBuf[1]);
			}
			
			return ;
		}
	}
	/////////////////////////////////////////////////////////
}

void CLanClient::sendPost(){
	
	stSession* session = _session;
	unsigned __int64 sessionID = session->_sessionID;

	/////////////////////////////////////////////////////////
	// 보낼 데이터가 있는지 체크
	/////////////////////////////////////////////////////////
	CLockFreeQueue<CPacketPointer>* sendQueue = &session->_sendQueue;
	int wsaNum;

	unsigned int usedSize = sendQueue->getSize();
	wsaNum = usedSize;
	wsaNum = min(wsaNum, MAX_PACKET);
	/////////////////////////////////////////////////////////

	if (wsaNum == 0) {
		return;
	}
	if (session->_released == true) {
		return;
	}

	/////////////////////////////////////////////////////////
	// send 1회 제한 처리
	/////////////////////////////////////////////////////////
	if (InterlockedExchange8((CHAR*)&session->_isSent, 1) == 1) {
		return;
	}
	/////////////////////////////////////////////////////////

	/////////////////////////////////////////////////////////
	// ioCnt 증가
	/////////////////////////////////////////////////////////
	InterlockedIncrement16((SHORT*)&session->_ioCnt);
	/////////////////////////////////////////////////////////


	/////////////////////////////////////////////////////////
	// packet을 wsaBuf로 복사
	/////////////////////////////////////////////////////////
	WSABUF wsaBuf[MAX_PACKET];
	session->_packetCnt = wsaNum;
	int packetNum = wsaNum;

	for(int packetCnt = 0; packetCnt < packetNum; ++packetCnt){
		
		CPacketPointer* packet = &session->_packets[packetCnt];

		sendQueue->pop(packet);
		wsaBuf[packetCnt].buf = packet->getBufStart();
		wsaBuf[packetCnt].len = packet->getPacketSize();
		packet->decRef();

	}
	/////////////////////////////////////////////////////////
	
	/////////////////////////////////////////////////////////
	// wsa send
	/////////////////////////////////////////////////////////
	int sendResult;
	int sendError;

	SOCKET sock = session->_sock;
	OVERLAPPED* overlapped = &session->_sendOverlapped;

	sendResult = WSASend(sock, wsaBuf, wsaNum, nullptr, 0, overlapped, nullptr);

	if(sendResult == SOCKET_ERROR){
		sendError = WSAGetLastError();
		if(sendError != WSA_IO_PENDING){
			Disconnect();
			session->_isSent = false;
			if (InterlockedDecrement16((SHORT*)&session->_ioCnt) == 0) {
				release();
			}
			_log(L"send.txt", LOG_GROUP::LOG_SYSTEM, L"session: 0x%I64x, sock: %I64d, wsaNum: %d, wsaBuf[0]: 0x%I64x, wsaBuf[1]: 0x%I64x, error: %d\n", sessionID, sock, wsaNum, wsaBuf[0], wsaBuf[1], sendError);
			return ;
		}
	}	
	/////////////////////////////////////////////////////////

}

bool CLanClient::Connect(const wchar_t* serverIP, unsigned short serverPort,
			int sendBufferSize, int recvBufferSize,
	int createWorkerThreadNum, int runningWorkerThreadNum){
		
	_sendBufferSize = sendBufferSize;
	_recvBufferSize = recvBufferSize;

	if (_session == nullptr) {
		_session = new stSession(sendBufferSize, recvBufferSize);
	}

	// iocp 초기화
	int iocpError;
	do {
		if (_iocp != nullptr) {
			break;
		}

		_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, runningWorkerThreadNum);
		if (_iocp == NULL) {
			iocpError = WSAGetLastError();
			CDump::crash();
		}
	} while (false);

	_session->_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int socketError;
	if (_session->_sock == INVALID_SOCKET) {

		socketError = WSAGetLastError();
		CDump::crash();

	}

	int ioctlResult;
	int ioctlError;
	u_long setNonBlock = 1;
	ioctlResult = ioctlsocket(_session->_sock, FIONBIO, &setNonBlock);
	if (ioctlResult == SOCKET_ERROR) {

		ioctlError = WSAGetLastError();
		CDump::crash();

	}

	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(serverPort);
	InetPtonW(AF_INET, serverIP, &addr.sin_addr.S_un.S_addr);
	
	connect(_session->_sock, (SOCKADDR*)&addr, sizeof(SOCKADDR_IN));

	Sleep(2000);

	fd_set set;
	FD_ZERO(&set);
	FD_SET(_session->_sock, &set);

	timeval time;
	time.tv_sec = 0;
	time.tv_usec = 0;
	select(0, nullptr, &set, nullptr, &time);

	if (FD_ISSET(_session->_sock, &set) == 0) {
		return false;
	}

	CreateIoCompletionPort((HANDLE)_session->_sock, (HANDLE)_iocp, (ULONG_PTR)1, 0);
	// worker thread 초기화
	{
		_workerThreadNum = createWorkerThreadNum;
		_workerThread = (HANDLE*)HeapAlloc(_heap, HEAP_ZERO_MEMORY, sizeof(HANDLE) * createWorkerThreadNum);

		for(int threadCnt = 0; threadCnt < createWorkerThreadNum ; ++threadCnt){
			_workerThread[threadCnt] = (HANDLE)_beginthreadex(nullptr, 0, CLanClient::completionStatusFunc, (void*)this, 0, nullptr);
		}

	}

	recvPost();

	return true;
}

void CLanClient::checkCompletePacket(stSession* session, CRingBuffer* recvBuffer){
	
	unsigned __int64 sessionID = session->_sessionID;
	unsigned int usedSize = recvBuffer->getUsedSize();

	while(usedSize > sizeof(stHeader)){
		
		// header 체크
		stHeader header;
		recvBuffer->frontBuffer(sizeof(stHeader), (char*)&header);

		int payloadSize = header.size;
		int packetSize = payloadSize + sizeof(stHeader);

		if (payloadSize != 8) {
			CDump::crash();
		}

		// 패킷이 recvBuffer에 완성되었다면
		if(usedSize >= packetSize){
			
			recvBuffer->popBuffer(sizeof(stHeader));

			CPacketPtr_Lan packet;

			recvBuffer->frontBuffer(payloadSize, packet.getRearPtr());
			packet.moveRear(payloadSize);

			recvBuffer->popBuffer(payloadSize);
			
			packet.moveFront(sizeof(stHeader));

			onRecv(packet);
			packet.decRef();

			usedSize -= packetSize;

		} else {
			break;
		}

	}
}

CLanClient::stSession::stSession(unsigned int sendQueueSize, unsigned int recvBufferSize):
	_recvBuffer(recvBufferSize)
{
	_sessionID = 0;
	_sock = NULL;
	_ip = 0;
	_port = 0;
	_isSent = false;
	_released = false;
	_ioCnt = 0;
	_callDisconnect = false;

	ZeroMemory(&_sendOverlapped, sizeof(OVERLAPPED));
	ZeroMemory(&_recvOverlapped, sizeof(OVERLAPPED));
	_packets = (CPacketPointer*)HeapAlloc(_heap, HEAP_ZERO_MEMORY, sizeof(CPacketPointer) * MAX_PACKET);
	_packetCnt = 0;

}

CLanClient::stSession::~stSession(){
	HeapFree(_heap, 0, _packets);
}