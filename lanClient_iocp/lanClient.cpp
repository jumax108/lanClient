

#include "headers/lanClient.h"

CLanClient::CLanClient(int maxPacketNum, int workerThreadNum): _recvBuffer(5000), _sendQueue(5000){

	_heap = HeapCreate(0,0,0);

	if(InitializeCriticalSectionAndSpinCount(&_lock, 0) == false){
		CDump::crash();
	}

	_sendCnt = 0;
	_recvCnt = 0;
	_sendTPS = 0;
	_recvTPS = 0;
	
	_tpsCalcThread = (HANDLE)_beginthreadex(nullptr, 0, tpsCalcFunc, this, 0, nullptr);
	
	_sendPosted = false;
	_disconnected = true;

	_packetNum = 0;
	_packetCnt = 0;
	_packets = nullptr;

	_workerThreadNum = 0;
	_workerThread = nullptr;

	_sock = NULL;
	_iocp = NULL;

	ZeroMemory(&_sendOverlapped, sizeof(OVERLAPPED));
	ZeroMemory(&_recvOverlapped, sizeof(OVERLAPPED));
	
	_ioCnt = 0;

	_stopEvent = CreateEvent(nullptr, true, false, nullptr);

	_packetNum = maxPacketNum;
	_packets = (CPacketPtr_Lan*)HeapAlloc(_heap, HEAP_ZERO_MEMORY, sizeof(CPacketPtr_Lan) * _packetNum);
	
	_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, workerThreadNum);
	int iocpError;
	if(_iocp == NULL){
		iocpError = GetLastError();
		CDump::crash();
	}

	_workerThreadNum = workerThreadNum;
	_workerThread = (HANDLE*)HeapAlloc(_heap, 0, sizeof(HANDLE) * _workerThreadNum);
	for(int threadCnt = 0; threadCnt < _workerThreadNum; ++threadCnt){
		_workerThread[threadCnt] = (HANDLE)_beginthreadex(nullptr, 0, completionStatusFunc, (void*)this, 0, nullptr);
	}
	
		

}
CLanClient::~CLanClient(){

	SetEvent(_stopEvent);
	if(WaitForSingleObject(_tpsCalcThread, INFINITE) == WAIT_FAILED){
		CDump::crash();
	}
	CloseHandle(_stopEvent);

	/* worker thread release */ {

		for(int workerThreadCnt = 0; workerThreadCnt < _workerThreadNum; ++workerThreadCnt){
			PostQueuedCompletionStatus(_iocp, 0, NULL, nullptr);
		}
		WaitForMultipleObjects(_workerThreadNum, _workerThread, true, INFINITE);
		HeapFree(_heap, 0, _workerThread);
		_workerThread = nullptr;
	}
	
	HeapFree(_heap, 0, _packets);
	_packets = nullptr;

	CloseHandle(_iocp);

	HeapDestroy(_heap);
	DeleteCriticalSection(&_lock);

}

unsigned __stdcall CLanClient::connectFunc(void* args){

	CLanClient* client = (CLanClient*)args;

	EnterCriticalSection(&client->_lock); {

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
					client->_disconnected = false;
					CreateIoCompletionPort((HANDLE)client->_sock, (HANDLE)client->_iocp, NULL, 0);			
					client->OnEnterJoinServer();
					break;
				} else if(connectError == WSAEALREADY){
					continue;
				} else if(connectError != WSAEWOULDBLOCK){
					client->OnError(connectError, L"Connect: Connect Error");
					LeaveCriticalSection(&client->_lock);
					return 1;
				}
			}
		}

		client->recvPost();

	} LeaveCriticalSection(&client->_lock);

	return 0;

}

bool CLanClient::Connect(const wchar_t* ip, unsigned short port, bool onNagle){
	
	EnterCriticalSection(&_lock); {
		
		WSAData wsaData;
		int startupError;
		if(WSAStartup(MAKEWORD(2,2), &wsaData) != 0){

			startupError = WSAGetLastError();
			OnError(startupError, L"Constructor: WSA Startup Error");
			LeaveCriticalSection(&_lock);
			return  false;
		}

		_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		int socketError;
		if(_sock == INVALID_SOCKET){

			socketError = WSAGetLastError();
			OnError(socketError, L"Connect: Socket Error");
			LeaveCriticalSection(&_lock);
			return false;

		}

		int ioctlResult;
		int ioctlError;
		u_long setNonBlock = 1;
		ioctlResult = ioctlsocket(_sock, FIONBIO, &setNonBlock);
		if(ioctlResult == SOCKET_ERROR){

			ioctlError = WSAGetLastError();
			OnError(ioctlError, L"Connect: Set Non Blocking Socket Error");
			LeaveCriticalSection(&_lock);
			return false;

		}

		int onNagleResult;
		int onNagleError;
		onNagleResult = setsockopt(_sock, IPPROTO_TCP, TCP_NODELAY, (const char*)&onNagle, sizeof(bool));
		if(onNagleResult == SOCKET_ERROR){

			onNagleError = WSAGetLastError();
			OnError(onNagleError, L"Connect: Nagle Option Set Error");
			LeaveCriticalSection(&_lock);
			return false;

		}

		_ip = ip;
		_port = port;

		_beginthreadex(nullptr, 0, connectFunc, (void*)this, 0, nullptr);
		
	} LeaveCriticalSection(&_lock);

	return true;
}

bool CLanClient::Disconnect(){

	EnterCriticalSection(&_lock); {

		_disconnected = true;

		closesocket(_sock);
		_sock = 0;

	} LeaveCriticalSection(&_lock);

	return true;
}

void CLanClient::release(){
	
	EnterCriticalSection(&_lock); {

		/* packet release */ {
			
			int sendQueueSize = _sendQueue.size();

			CPacketPtr_Lan packet;
			packet.decRef();

			for(int packetCnt = 0; packetCnt < sendQueueSize; ++packetCnt){
				_sendQueue.front(&packet);
				packet.decRef();
				packet.~CPacketPtr_Lan();
				_sendQueue.pop();
			}

			printf("%d\n", sendQueueSize);

			_recvBuffer.moveFront(_recvBuffer.getUsedSize());

		}
		
		_sendPosted = false;

		WSACleanup();

		OnLeaveServer();

	} LeaveCriticalSection(&_lock);
}

bool CLanClient::sendPacket(CPacketPtr_Lan packet){
	
	EnterCriticalSection(&_lock); {

		if(_disconnected == true){
			LeaveCriticalSection(&_lock);
			packet.decRef();
			packet.~CPacketPtr_Lan();
			return false;
		}

		packet.setHeader();

		packet.incRef();
		_sendQueue.push(packet);

		if(_sendPosted == false){
			sendPost();
		}

	} LeaveCriticalSection(&_lock);

	return true;

}

unsigned CLanClient::completionStatusFunc(void *args){
	
	CLanClient* client = (CLanClient*)args;
	CRITICAL_SECTION* lock = &client->_lock;
	CQueue<CPacketPtr_Lan>* sendQueue = &client->_sendQueue;
	CRingBuffer* recvBuffer = &client->_recvBuffer;
	CPacketPtr_Lan* packets = client->_packets;

	HANDLE iocp = client->_iocp;

	for(;;){
		
		unsigned int transferred;
		unsigned __int64 sessionID;
		OVERLAPPED* overlapped;
		GetQueuedCompletionStatus(iocp, (LPDWORD)&transferred, (PULONG_PTR)&sessionID, &overlapped, INFINITE);
		
		EnterCriticalSection(lock); {

			if(overlapped == nullptr){
				LeaveCriticalSection(lock);
				break;			
			}
			
			if(&client->_sendOverlapped == overlapped){
		
				int packetNum = client->_packetCnt;
				CPacketPtr_Lan* packetIter = packets;
				CPacketPtr_Lan* packetEnd = packets + packetNum;

				int packetTotalSize = 0;

				for(; packetIter != packetEnd; ++packetIter){
					packetTotalSize += packetIter->getPacketSize();
					packetIter->decRef();
					packetIter->~CPacketPtr_Lan();
				}
			
				client->_packetCnt = 0;
				InterlockedAdd((LONG*)&client->_sendCnt, packetNum);
				
				client->OnSend(packetTotalSize);

				if(sendQueue->size() != 0){
					client->sendPost();
				} else {
					client->_sendPosted = false;
				}
			
			}

			else if(&client->_recvOverlapped == overlapped){

				// recv ¿Ï·á
				recvBuffer->moveRear(transferred);

				// packet proc
				client->checkCompletePacket(sessionID, recvBuffer);

				client->recvPost();
			
			}

			client->_ioCnt -= 1;
			if(client->_ioCnt == 0){
				client->release();
			}

		} LeaveCriticalSection(lock);

	}

	return 0;
}

void CLanClient::recvPost(){
	
	if(_disconnected == true){
		return;
	}

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

	_ioCnt += 1;
	recvResult = WSARecv(sock, wsaBuf, wsaCnt, nullptr, (LPDWORD)&flag, overlapped, nullptr);
	if(recvResult == SOCKET_ERROR){
		recvError = WSAGetLastError();
		if(recvError != WSA_IO_PENDING){
			OnError(recvError, L"RecvPost: Recv Error");
			Disconnect();
			_ioCnt -= 1;
			return ;
		}
	}
}

void CLanClient::sendPost(){

	if(_disconnected == true){
		_sendPosted = false;
		return ;
	}

	_sendPosted = true;

	CQueue<CPacketPtr_Lan>* sendQueue = &_sendQueue;
	int wsaNum;

	unsigned int usedSize = sendQueue->size();
	wsaNum = usedSize;
	wsaNum = min(wsaNum, _packetNum);

	OVERLAPPED* overlapped = &_sendOverlapped;
	
	WSABUF wsaBuf[100];
	
	_packetCnt = wsaNum;

	int packetNum = wsaNum;

	CPacketPtr_Lan packet;
	packet.decRef();

	for(int packetCnt = 0; packetCnt < packetNum; ++packetCnt){
		
		sendQueue->front(&packet);
		sendQueue->pop();
		wsaBuf[packetCnt].buf = packet.getBufStart();
		wsaBuf[packetCnt].len = packet.getPacketSize();

		packet.decRef();
		packet.~CPacketPtr_Lan();

		_packets[packetCnt] = packet;

	}

	int sendResult;
	int sendError;

	SOCKET sock = _sock;

	_ioCnt += 1;
	sendResult = WSASend(sock, wsaBuf, wsaNum, nullptr, 0, overlapped, nullptr);
	if(sendResult == SOCKET_ERROR){
		sendError = WSAGetLastError();
		if(sendError != WSA_IO_PENDING){
			OnError(sendError, L"SendPost: Send Error");
			Disconnect();
			_sendPosted = false;
			_ioCnt -= 1;
			return ;
		}
	}	
}
void CLanClient::checkCompletePacket(unsigned __int64 sessionID, CRingBuffer* recvBuffer){
	
	unsigned int usedSize = recvBuffer->getUsedSize();

	while(usedSize > sizeof(stHeader)){
		
		stHeader header;

		recvBuffer->frontBuffer(sizeof(stHeader), (char*)&header);

		int payloadSize = header.size;
		int packetSize = payloadSize + sizeof(stHeader);

		if(usedSize >= packetSize){
			
			recvBuffer->popBuffer(sizeof(stHeader));

			CPacketPtr_Lan packet;

			memcpy(packet.getBufStart(), &header.size, sizeof(stHeader::size));
			recvBuffer->frontBuffer(payloadSize, packet.getRearPtr());
			packet.moveRear(payloadSize);

			recvBuffer->popBuffer(payloadSize);

			packet.moveFront(sizeof(stHeader));

			InterlockedIncrement((LONG*)&_recvCnt);

			OnRecv(packet);

			packet.decRef();

			usedSize -= packetSize;

		} else {
			break;
		}

	}
}

unsigned __stdcall CLanClient::tpsCalcFunc(void* args){

	CLanClient* client = (CLanClient*)args;

	int* sendCnt = &client->_sendCnt;
	int* recvCnt = &client->_recvCnt;

	int* sendTPS = &client->_sendTPS;
	int* recvTPS = &client->_recvTPS;

	HANDLE* stopEvent = &client->_stopEvent;

	for(;;){

		int stopEventReturn = WaitForSingleObject(*stopEvent, 0);
		if(stopEventReturn == WAIT_OBJECT_0){
			break;
		} else if(stopEventReturn == WAIT_FAILED){
			CDump::crash();
		}

		*sendTPS = *sendCnt;
		*recvTPS = *recvCnt;

		InterlockedExchange((LONG*)sendCnt, 0);
		InterlockedExchange((LONG*)recvCnt, 0);

		Sleep(999);

	}

	return 0;

}