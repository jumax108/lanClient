

#include "headers/lanClient.h"

CLanClient::CLanClient(const wchar_t* ip, unsigned short port, 
	bool onNagle, int maxPacketNum, int workerThreadNum):
	_recvBuffer(5000), _sendQueue(5000){

	/* heap setting */ {
		_heap = HeapCreate(0,0,0);
	}

	/* 동기화 객체 세팅 */ {
		if(InitializeCriticalSectionAndSpinCount(&_lock, 0) == false){
			CDump::crash();
		}
	}

	/* 데이터 세팅 */ {
		
		_ip = ip;
		_port = port;
		_onNagle = onNagle;

		_sendCnt = 0;
		_recvCnt = 0;
		_sendTPS = 0;
		_recvTPS = 0;
	
		_sendPosted = false;
		_disconnected = true;
		
		_packetNum = maxPacketNum;
		_packetCnt = 0;
		_packets = (CPacketPtr_Lan*)HeapAlloc(_heap, HEAP_ZERO_MEMORY, sizeof(CPacketPtr_Lan) * _packetNum);

		_workerThreadNum = workerThreadNum;
		_workerThread = nullptr;
	
		_sock = NULL;
		
		_ioCnt = 0;
		
		ZeroMemory(&_sendOverlapped, sizeof(OVERLAPPED));
		ZeroMemory(&_recvOverlapped, sizeof(OVERLAPPED));

		logIndex = 0;
		ZeroMemory(_log, sizeof(_log));

	}
	
	/* 이벤트 세팅 */ {
		_stopEvent = CreateEvent(nullptr, true, false, nullptr);
		_connectEvent = CreateEvent(nullptr, false, false, nullptr);
	}
	
	/* 네트워크 세팅 */ {

		_iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, NULL, workerThreadNum);
		int iocpError;
		if(_iocp == NULL){
			iocpError = GetLastError();
			CDump::crash();
		}

		WSAData wsaData;
		int startupError;
		if(WSAStartup(MAKEWORD(2,2), &wsaData) != 0){

			startupError = WSAGetLastError();
			CDump::crash();
		}

		

	}

	/* 스레드 세팅 */ {
		
		_tpsCalcThread = (HANDLE)_beginthreadex(nullptr, 0, tpsCalcFunc, this, 0, nullptr);
		
		_workerThread = (HANDLE*)HeapAlloc(_heap, 0, sizeof(HANDLE) * _workerThreadNum);
		for(int threadCnt = 0; threadCnt < _workerThreadNum; ++threadCnt){
			_workerThread[threadCnt] = (HANDLE)_beginthreadex(nullptr, 0, completionStatusFunc, (void*)this, 0, nullptr);
		}
		
		_connectThread = (HANDLE)_beginthreadex(nullptr, 0, connectFunc, (void*)this, 0, nullptr);
	
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

	WSACleanup();

	HeapDestroy(_heap);

	DeleteCriticalSection(&_lock);

}

void CLanClient::Connect() {
	
	_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	int socketError;
	if(_sock == INVALID_SOCKET){

		socketError = WSAGetLastError();
		CDump::crash();

	}



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

			for(int packetCnt = 0; packetCnt < sendQueueSize; ++packetCnt){
				CPacketPtr_Lan* packet = &_packets[packetCnt];

				_sendQueue.front(packet);
				_sendQueue.pop();

				packet->decRef();
				packet->~CPacketPtr_Lan();
			}
			
			_recvBuffer.moveFront(_recvBuffer.getUsedSize());

		}
		
		_sendPosted = false;

		OnLeaveServer();

	} LeaveCriticalSection(&_lock);
}

bool CLanClient::sendPacket(CPacketPtr_Lan packet){
	
	EnterCriticalSection(&_lock); {

		_log[logIndex++].msg = (wchar_t*)L"enter sendPacket";

		if(_disconnected == true){
			packet.decRef();
			LeaveCriticalSection(&_lock);
			return false;
		}

		packet.setHeader();

		packet.incRef();
		_sendQueue.push(packet);

		if(_sendPosted == false){
			sendPost();
		}

		_log[logIndex++].msg = (wchar_t*)L"leave sendPacket";

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

			/*if(overlapped == nullptr){
				LeaveCriticalSection(lock);
				break;			
			}*/
			
			if(&client->_sendOverlapped == overlapped){
		
				client->_log[client->logIndex++].msg = (wchar_t*)L"enter send completion";

				int packetNum = client->_packetCnt;
				CPacketPtr_Lan* packetIter = packets;
				CPacketPtr_Lan* packetEnd = packets + packetNum;

				int packetTotalSize = 0;

				for(; packetIter != packetEnd; ++packetIter){
					packetTotalSize += packetIter->getPacketSize();

					packetIter->decRef();
					/*
				//	packetIter->~CPacketPtr_Lan();

					if(packetIter->_packet != nullptr){
						CDump::crash();
					}
					*/
				}
			
				client->_packetCnt = 0;
				InterlockedAdd((LONG*)&client->_sendCnt, packetNum);
				
				client->OnSend(packetTotalSize);

				if(sendQueue->size() != 0){
					client->sendPost();
				} else {
					client->_sendPosted = false;
				}
			
				client->_log[client->logIndex++].msg = (wchar_t*)L"leave send completion";
			}

			else if(&client->_recvOverlapped == overlapped){
				
				if(client->_firstRecv == true){

					char* recv = recvBuffer->getRearPtr();
					recv += 2;
					for(int i=0 ; i<8 ; i++){
						if(*recv != 0){
							CDump::crash();
						}
					}
					client->_firstRecv = false;
				}

				client->_log[client->logIndex++].msg = (wchar_t*)L"enter recv completion";
				// recv 완료
				recvBuffer->moveRear(transferred);

				// packet proc
				client->checkCompletePacket(sessionID, recvBuffer);

				client->recvPost();
				client->_log[client->logIndex++].msg = (wchar_t*)L"leave recv completion";
			
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
	
	_log[logIndex++].msg = (wchar_t*)L"enter recv Post";

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
		_log[logIndex++].msg = (wchar_t*)L"leave recv Post";
}

void CLanClient::sendPost(){
	
	_log[logIndex++].msg = (wchar_t*)L"enter send Post";

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

	for(int packetCnt = 0; packetCnt < packetNum; ++packetCnt){
		
		CPacketPtr_Lan* packet = &_packets[packetCnt];

		sendQueue->front(packet);
		sendQueue->pop();
		wsaBuf[packetCnt].buf = packet->getBufStart();
		wsaBuf[packetCnt].len = packet->getPacketSize();
		/*
		unsigned short size;
		*packet >> size;
		unsigned int data;
		*packet >> data;
		printf("%d\n",data);
		*/
		packet->decRef();
	}

	int sendResult ;
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
	
	_log[logIndex++].msg = (wchar_t*)L"leave send Post";
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
			
				_log[logIndex++].msg = (wchar_t*)L"enter on recv";
			OnRecv(packet);
				_log[logIndex++].msg = (wchar_t*)L"leave on recv";

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