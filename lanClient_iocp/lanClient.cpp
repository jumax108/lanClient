#include "lanClient.h"

CLanClient::CLanClient(): _recvBuffer(5000){

	_heap = HeapCreate(0,0,0);

	init();
	
}

void CLanClient::init(){
	
	_sessionID = 0;

	_sendPosted = false;

	_packetNum = 0;
	_packetCnt = 0;
	_packets = nullptr;

	_workerThreadNum = 0;
	_workerThread = nullptr;

	_sock = NULL;

	_iocp = NULL;

	ZeroMemory(&_sendOverlapped, sizeof(OVERLAPPED));
	ZeroMemory(&_recvOverlapped, sizeof(OVERLAPPED));

}

unsigned __stdcall CLanClient::connectFunc(void* args){

	CLanClient* client = (CLanClient*)args;

	SOCKADDR_IN addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(client->_port);
	InetPtonW(AF_INET, client->_ip, &addr.sin_addr.S_un.S_addr);

	int connectError;
	int connectResult;
	for(;;){
		connectResult = connect(client->_sock, (SOCKADDR*)&addr, sizeof(SOCKADDR_IN));
		if(connectResult == SOCKET_ERROR){
		
			connectError = WSAGetLastError();
			if(connectError == WSAEISCONN){
				CreateIoCompletionPort((HANDLE)client->_sock, (HANDLE)client->_iocp, NULL, 0);
				client->OnEnterJoinServer();
				break;
			} else if(connectError != WSAEWOULDBLOCK){
				client->OnError(connectError, L"Connect: Connect Error");
				return 1;
			} 

		}
	}


	client->recvPost();


	return 0;

}

bool CLanClient::Connect(const wchar_t* ip, unsigned short port, int maxPacketNum, int workerThreadNum, bool onNagle){

	WSAData wsaData;
	int startupError;
	if(WSAStartup(MAKEWORD(2,2), &wsaData) != 0){

		startupError = WSAGetLastError();
		OnError(startupError, L"Connect: WSA Startup Error");
		return false;

	}

	_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int socketError;
	if(_sock == INVALID_SOCKET){

		socketError = WSAGetLastError();
		OnError(socketError, L"Connect: Socket Error");
		return false;

	}

	int ioctlResult;
	int ioctlError;
	u_long setNonBlock = 1;
	ioctlResult = ioctlsocket(_sock, FIONBIO, &setNonBlock);
	if(ioctlResult == SOCKET_ERROR){

		ioctlError = WSAGetLastError();
		OnError(ioctlError, L"Connect: Set Non Blocking Socket Error");
		return false;

	}

	int onNagleResult;
	int onNagleError;
	onNagleResult = setsockopt(_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&onNagle, sizeof(bool));
	if(onNagleResult == SOCKET_ERROR){

		onNagleError = WSAGetLastError();
		OnError(onNagleError, L"Connect: Nagle Option Set Error");
		return false;

	}

	_ip = ip;
	_port = port;

	_beginthreadex(nullptr, 0, connectFunc, (void*)this, 0, nullptr);

	_packetNum = maxPacketNum;
	_packets = (CPacketPtr*)HeapAlloc(_heap, 0, sizeof(CPacketPtr) * _packetNum);
	
	_workerThreadNum = workerThreadNum;
	_workerThread = (HANDLE*)HeapAlloc(_heap, 0, sizeof(HANDLE) * _workerThreadNum);
	for(int threadCnt = 0; threadCnt < _workerThreadNum; ++threadCnt){
		_workerThread[threadCnt] = (HANDLE)_beginthreadex(nullptr, 0, completionStatusFunc, (void*)this, 0, nullptr);
	}

	_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, workerThreadNum);
	int iocpError;
	if(_iocp == NULL){
		
		iocpError = GetLastError();
		OnError(iocpError, L"Connect: IOCP Create Error");
		return false;

	}

	return true;
}

bool CLanClient::Disconnect(){

	closesocket(_sock);

	HeapFree(_heap, 0, _packets);
	_packets = nullptr;

	HeapFree(_heap, 0, _workerThread);
	_workerThread = nullptr;

	init();

	return true;
}

bool CLanClient::sendPacket(CPacketPtrLan packet){
	
	packet.setHeader();

	packet.incRef();
	_sendQueue.push(packet);

	bool sendPosted = InterlockedExchange8((CHAR*)&_sendPosted, true);
	if(sendPosted == false){
		sendPost();
	}

	return true;

}

unsigned CLanClient::completionStatusFunc(void *args){
	
	CLanClient* client = (CLanClient*)args;

	HANDLE iocp = client->_iocp;

	while(1){
		
		unsigned int transferred;
		unsigned __int64 sessionID;
		OVERLAPPED* overlapped;
		GetQueuedCompletionStatus(iocp, (LPDWORD)&transferred, (PULONG_PTR)&sessionID, &overlapped, INFINITE);
		
		//printf("OVERLAPPED: %I64x\n", overlapped);

		if(overlapped == nullptr){
			break;			
		}
			
		if(&client->_sendOverlapped == overlapped){
		
			int packetNum = client->_packetCnt;
			CPacketPtr* packets = client->_packets;
			CPacketPtr* packetIter = packets;
			CPacketPtr* packetEnd = packets + packetNum;
			for(; packetIter != packetEnd; ++packetIter){
				packetIter->decRef();
			}
			
			client->_packetCnt = 0;
			
			InterlockedExchange8((char*)&client->_sendPosted, false);
			
			CLockFreeQueue<CPacketPtr>* sendQueue = &client->_sendQueue;
			
			//printf("%I64d\n", sendQueue->getSize());
			if(sendQueue->getSize() != 0){

				bool sendProcessing = InterlockedExchange8((char*)&client->_sendPosted, true);
				if(sendProcessing == false){
					client->sendPost();
				}
			}
			
		}

		if(&client->_recvOverlapped == overlapped){

			// recv ¿Ï·á
			CRingBuffer* recvBuffer = &client->_recvBuffer;

			recvBuffer->moveRear(transferred);

			// packet proc
			client->checkCompletePacket(sessionID, recvBuffer);

			client->recvPost();
			
		}

	}

	return 0;
}

void CLanClient::recvPost(){
	
	OVERLAPPED* overlapped = &_recvOverlapped;
	
	WSABUF wsaBuf[2];
	int wsaCnt = 1;

	CRingBuffer* recvBuffer = &_recvBuffer;

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

	int recvResult;
	int recvError;
	
	SOCKET sock = _sock;
	unsigned int flag = 0;
			//printf("RECV: %d\n", sock);
	recvResult = WSARecv(sock, wsaBuf, wsaCnt, nullptr, (LPDWORD)&flag, overlapped, nullptr);
	if(recvResult == SOCKET_ERROR){
		recvError = WSAGetLastError();
		if(recvError != WSA_IO_PENDING){
			OnError(recvError, L"RecvPost: Recv Error");
			Disconnect();
			return ;
		}
	}
}

void CLanClient::sendPost(){

	CLockFreeQueue<CPacketPtr>* sendQueue = &_sendQueue;
	int wsaNum;

	unsigned int usedSize = sendQueue->getSize();
	wsaNum = usedSize;
	wsaNum = min(wsaNum, _packetNum);

	if(wsaNum == 0){

		InterlockedExchange8((char*)&_sendPosted, false);

	}

	OVERLAPPED* overlapped = &_sendOverlapped;
	
	
	WSABUF wsaBuf[100];
	
	_packetCnt = wsaNum;

	int packetNum = wsaNum;

	CPacketPtr packet;

	for(int packetCnt = 0; packetCnt < packetNum; ++packetCnt){
		
		sendQueue->pop(&packet);
		wsaBuf[packetCnt].buf = packet.getBufStart();
		wsaBuf[packetCnt].len = packet.getPacketSize();
		packet.decRef();

		_packets[packetCnt] = packet;

	}

	int sendResult;
	int sendError;

	SOCKET sock = _sock;

	
		//	printf("SEND: %d\n", sock);
	sendResult = WSASend(sock, wsaBuf, wsaNum, nullptr, 0, overlapped, nullptr);
	//		printf("SEND RESULT: %d\n", sendResult);
	//		printf("send overlapped: %I64x\n", overlapped);
	if(sendResult == SOCKET_ERROR){
		sendError = WSAGetLastError();
		if(sendError != WSA_IO_PENDING){
			Disconnect();
			
			return ;
		}
	}	
}

void CLanClient::checkCompletePacket(unsigned __int64 sessionID, CRingBuffer* recvBuffer){
	
	unsigned int usedSize = recvBuffer->getUsedSize();

	while(usedSize > sizeof(stHeader)){
		
		stHeader header;

		recvBuffer->front(sizeof(stHeader), (char*)&header);

		int payloadSize = header.size;
		int packetSize = payloadSize + sizeof(stHeader);

		if(usedSize >= packetSize){
			
			recvBuffer->pop(sizeof(stHeader));

			CPacketPtrLan packet;
			//packet << header.size;
			memcpy(packet.getBufStart(), &header.size, sizeof(stHeader::size));
			recvBuffer->front(payloadSize, packet.getRearPtr());
			packet.moveRear(payloadSize);

			recvBuffer->pop(payloadSize);

			packet.moveFront(sizeof(stHeader));

			OnRecv(packet);

			packet.decRef();

			usedSize -= packetSize;

		} else {
			break;
		}

	}
}