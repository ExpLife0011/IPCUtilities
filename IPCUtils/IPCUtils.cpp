// IPCUtils.cpp : Defines the exported functions for the DLL application.
//

#include <cmath>

#include "stdafx.h"
#include "IPCUtils.h"


IPCManager IPCManager::instance;
std::mutex IPCManager::innerLocker;
const size_t IPCManager::IPC_MEMORY_SIZE = sizeof(IPCManager::IPCSharedMemoryPage);
const uint32_t MAX_WAIT_TIMEOUT = 0xffffffff;

const wchar_t* IPCFileMappingName = L"Scorp/IPCFileMapping";
const wchar_t* IPCManagerSemaName = L"Scorp/IPCManagerSema";
const wchar_t* IPCDataSemaName = L"Scorp/IPCDataSema";
//const wchar_t* IPCControlSemaName = L"Scorp/IPCControlSema";


int __stdcall InitWithMode(int Mode, ReceiverCallback rCallback, SenderCallback sCallback) {
	int ret = 0;
	switch (Mode) {
		case 0: {
			IPCManager::getInstance().initWithReceiverMode(rCallback);
		}break;
		case 1: {
			IPCManager::getInstance().initWithSenderMode(sCallback);
		}break;
		case 2: {
			IPCManager::getInstance().initWithDualMode(rCallback,sCallback);
		}break;
		default: {
			ret = -1;
		}
	}

#ifdef _DEBUG
	std::cout << "Init with mode " << Mode << std::endl;
#endif

	return ret;
}

void __stdcall SendIPCMsg(char* value, int length) {
	if (length > IPCMSG_CAP) {
		return;
	}
	IPCManager::getInstance().loadSendingMsg(value, length);
}

int __stdcall QueryRecvInfo() {
	return IPCManager::getInstance().checkRecvFlag();
}
int __stdcall RetriveRecvMsg(char* value, int length) {
	return IPCManager::getInstance().retriveRecvMsg(value, length);
}

void __stdcall ActiveDebugWindow() {
	if (::AllocConsole()) {
		::freopen("CONIN$", "r+t", stdin);
		::freopen("CONOUT$", "w+t", stdout);
		//::freopen("CONOUT$","w+t",stderr);
		::SetConsoleTitle(L"IPCManager Debug");
		::SetConsoleTextAttribute(::GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED);
	}
}

/////IPCManager

IPCManager& IPCManager::getInstance() {
	return IPCManager::instance;
}

IPCManager::IPCManager()  {
	mode = RunningMode::Uninitialized;
	receiverCallback	= nullptr;
	senderCallback		= nullptr;
	ipcSema				= nullptr;
	ipcMapFile			= nullptr;
	ipcReceiverTid		= nullptr;
	ipcSenderTid		= nullptr;
	ipcData				= nullptr;
	
	::ZeroMemory(ipcMsgBuffer, IPCMSG_CAP * sizeof(char));
}

IPCManager::~IPCManager() {

	innerLocker.lock();
	ipcData->RunningInfo = -1;
	innerLocker.unlock();

	if (ipcData) {
		delete ipcData;
	}

	::UnmapViewOfFile(ipcMapView);
	::CloseHandle(ipcMapFile);
	::CloseHandle(ipcSema);

}

bool IPCManager::hasInit()const {
	return (mode != RunningMode::Uninitialized);
}

void IPCManager::initWithReceiverMode(ReceiverCallback rcb) {
	if (!hasInit()) {
		return;
	}
	mode = RunningMode::ReceiverMode;
	receiverCallback = rcb;

	prepareSema();
	//initReceiverProc();
	run();
}

void IPCManager::initWithSenderMode(SenderCallback scb) {
	if (!hasInit()) {
		return;
	}
	mode = RunningMode::SenderMode;
	senderCallback = scb;

	prepareSema();
	//initSenderProc();
	run();
}

void IPCManager::initWithDualMode(ReceiverCallback rcb, SenderCallback scb) {
	if (!hasInit()) {
		return;
	}
	mode = RunningMode::DualMode;
	receiverCallback = rcb;
	senderCallback = scb;

	prepareSema();
	//initSenderProc();
	//initReceiverProc();
	run();
}

int IPCManager::checkRecvFlag() {

	int ret = -1;

	::WaitForSingleObject(this->ipcData->ipcDataSema, MAX_WAIT_TIMEOUT);

	ret = reinterpret_cast<pIPCSharedMemoryPage>(ipcMapView)->receiverUpdateFlag;

	::ReleaseSemaphore(this->ipcData->ipcDataSema, 1, 0);

	return ret;
}

int IPCManager::retriveRecvMsg(char* buffer, int maxLen) {
	if (!(IPCManager::getInstance().hasInit())) {
		return -1;
	}

	std::lock_guard<std::mutex> lck(IPCManager::innerLocker);
	int copyLen = maxLen < IPCMSG_CAP ? maxLen : IPCMSG_CAP;
	::CopyMemory(buffer, this->ipcMsgBuffer, copyLen);
	return copyLen;
}

void IPCManager::loadSendingMsg(char* value, int length) {
	if (!(IPCManager::getInstance().hasInit())) {
		return;
	}
	
	std::lock_guard<std::mutex> lck(IPCManager::innerLocker);
	::CopyMemory(ipcMsgBuffer, value, length);
	lck.~lock_guard();


	pIPCSharedMemoryPage mPage = reinterpret_cast<pIPCSharedMemoryPage>(ipcMapView);
	bool canRelease = false;
	if (WAIT_OBJECT_0 == ::WaitForSingleObject(ipcData->ipcDataSema, 1000)) {	
		mPage->senderUpdateFlag = 1;
		canRelease = true;
	}
	::ReleaseSemaphore(ipcData->ipcDataSema, 1, 0);

	if (canRelease) {
		::ReleaseSemaphore(ipcData->ipcControlSema, 1, 0);
	}
}


void IPCManager::prepareSema() {

	//used to lock the initialization phase
	ipcSema = CreateSemaphore(NULL, 1, 1, IPCManagerSemaName);
	if (ipcSema != NULL) {

		if (ERROR_OBJECT_ALREADY_EXISTS == ::GetLastError()) {
			//other process has registered this already
		}

		::WaitForSingleObject(ipcSema, 0xffffffff);

		bool needZeroMem = false;

		//semaphore has been created by other process already
		{
			//This logic relies on the fact that:
			//if the object does not already exist, 
			//the OpenFileMapping function will fail.
			//This is useful in a DLL where the DLL's initialization code is called repeatedly, 
			//once for every process that attaches to it.

			//detect if the FileMapping has been created or not;
#ifdef _DEBUG
			std::cout << "Opening FileMapping" << std::endl;
#endif
			ipcMapFile = ::OpenFileMapping(
				FILE_MAP_ALL_ACCESS,
				FALSE,
				IPCFileMappingName
			);

			if (ipcMapFile == NULL) {
				needZeroMem = true;
#ifdef _DEBUG
				std::cout << "Creating FileMapping" << std::endl;
#endif
				ipcMapFile = ::CreateFileMapping(
					INVALID_HANDLE_VALUE,
					NULL,
					PAGE_READWRITE,
					0,
					IPC_MEMORY_SIZE,
					IPCFileMappingName
				);

				if (ipcMapFile == NULL) {
					DWORD errret = ::GetLastError();
#ifdef _DEBUG
					std::cout << "Unable to Create FileMapping! Error Code :"<<errret << std::endl;
#endif
				}
				else {
#ifdef _DEBUG
					std::cout << "Create FileMapping Done" << std::endl;
#endif
				}
			}
		}

#ifdef _DEBUG
		std::cout << "Creating MapView..." << std::endl;
#endif

		ipcMapView = MapViewOfFile(ipcMapFile,				// handle to map object
			FILE_MAP_ALL_ACCESS,	// read/write permission
			0,
			0,
			IPC_MEMORY_SIZE);
		
		if (needZeroMem) {
			::ZeroMemory(ipcMapView, IPC_MEMORY_SIZE);
		}

		::ReleaseSemaphore(ipcSema, 1, 0);
	}
	else {
		//TODO : Create Semaphore Failed

	}

#ifdef _DEBUG
	std::cout << "Create MapView Done!" << std::endl;
#endif

#ifdef _DEBUG
	std::cout << "Creating Control & Data Semaphores" << std::endl;
#endif

	auto ipcDataSema = ::CreateSemaphore(NULL, 0, 1, IPCDataSemaName);
	auto ipcControlSema = ::CreateSemaphore(NULL, 0, 1, NULL);

#ifdef _DEBUG
	std::cout << "Create Semaphore Done" << std::endl;
#endif
	if (ipcDataSema == NULL || ipcControlSema == NULL) {
		//something wrong
	}

#ifdef _DEBUG
	std::cout << "Preparing IPC Control Data" << std::endl;
#endif
	if (ipcData) {
		delete ipcData;
	}
	ipcData = new AsyncIPCData;
	
	ipcData->ipcDataSema = ipcDataSema;
	ipcData->ipcControlSema = ipcControlSema;
	ipcData->ipcMapFile = ipcMapFile;
	ipcData->ipcMemorySize = IPC_MEMORY_SIZE;
	ipcData->RunningInfo = 0;

	ipcData->receiverCallback = [this](int offset, int length)->void {
		std::lock_guard<std::mutex> lck(IPCManager::innerLocker);
		auto data = reinterpret_cast<pIPCSharedMemoryPage>(this->ipcMapView);

		if (data->receiverUpdateFlag != 0) {
			data->timestamp = time(nullptr);
			::CopyMemory(this->ipcMsgBuffer, data->rawData + offset, length);
			data->receiverUpdateFlag = 0;
		}
	};

	ipcData->senderCallback = [this](void)->int {
		std::lock_guard<std::mutex> lck(IPCManager::innerLocker);
		auto data = reinterpret_cast<pIPCSharedMemoryPage>(this->ipcMapView);
		
		if (data->senderUpdateFlag != 0){
			data->receiverUpdateFlag = 1;
			data->timestamp = time(nullptr);
			::CopyMemory(data->rawData, this->ipcMsgBuffer, IPCManager::IPC_MEMORY_SIZE);
			data->senderUpdateFlag = 0;
		}
		return 0;
	};
#ifdef _DEBUG
	std::cout << "Selecting Mode" << std::endl;
#endif
	switch (mode) {
		case RunningMode::ReceiverMode: {
			ipcData->senderCallback = nullptr;
		}break;
		case RunningMode::SenderMode: {
			ipcData->receiverCallback = nullptr;
		}break;
		case RunningMode::DualMode: {

		}break;
		default: {

		}break;
	}

}

void IPCManager::run() {
	switch (mode) {
		case RunningMode::ReceiverMode: {
			this->initReceiverProc();
		}break;
		case RunningMode::SenderMode: {
			this->initSenderProc();
		}break;
		default: {

		}break;
	}

}

void IPCManager::initReceiverProc() {

	ipcReceiverTid = ::CreateThread(
							NULL,
							(DWORD)NULL,
							(LPTHREAD_START_ROUTINE)(IPCManager::ReceiverProc),
							(LPVOID)(ipcData),
							(DWORD)NULL,
							NULL);
}

void IPCManager::initSenderProc() {
	
	ipcSenderTid = ::CreateThread(
							NULL,
							(DWORD)NULL,
							(LPTHREAD_START_ROUTINE)(IPCManager::SenderProc),
							(LPVOID)(ipcData),
							(DWORD)NULL,
							NULL);

}

void* IPCManager::ReceiverProc(void* data) {

	auto ipcData = reinterpret_cast<pAsyncIPCData>(data);

	while (ipcData->RunningInfo != 0) {

		::WaitForSingleObject(ipcData->ipcControlSema, MAX_WAIT_TIMEOUT);

		::WaitForSingleObject(ipcData->ipcDataSema, MAX_WAIT_TIMEOUT);

		ipcData->receiverCallback(0, IPCManager::IPC_MEMORY_SIZE);

		::ReleaseSemaphore(ipcData->ipcDataSema, 1, 0);

	}

	::CloseHandle(ipcData->ipcControlSema);
	::CloseHandle(ipcData->ipcDataSema);

	return nullptr;
}

void* IPCManager::SenderProc(void* data) {

	auto ipcData = reinterpret_cast<pAsyncIPCData>(data);

	while (ipcData->RunningInfo != 0) {

		::WaitForSingleObject(ipcData->ipcControlSema, MAX_WAIT_TIMEOUT);

		::WaitForSingleObject(ipcData->ipcDataSema, MAX_WAIT_TIMEOUT);
	
		ipcData->senderCallback();
	
		::ReleaseSemaphore(ipcData->ipcDataSema, 1, 0);
	}

	::CloseHandle(ipcData->ipcControlSema);
	::CloseHandle(ipcData->ipcDataSema);

	return nullptr;
}