/*
Copyleft (c) 2013-2015 EasyDarwin.ORG.  All rights reserved.
Github: https://github.com/EasyDarwin
WEChat: EasyDarwin
Website: http://www.EasyDarwin.org
*/
/*
File:       HTTPSession.cpp
Contains:   实现对服务单元每一个Session会话的网络报文处理
*/

#include "HTTPSession.h"
#include "QTSServerInterface.h"
#include "OSMemory.h"
#include "EasyUtil.h"

#include "OSArrayObjectDeleter.h"
#include <boost/algorithm/string.hpp>
#include "QueryParamList.h"

#if __FreeBSD__ || __hpux__	
#include <unistd.h>
#endif

#include <errno.h>

#if __solaris__ || __linux__ || __sgi__	|| __hpux__
#include <crypt.h>
#endif

using namespace std;

static const int sWaitDeviceRspTimeout = 10;


HTTPSession::HTTPSession( )
	: HTTPSessionInterface(),
	fRequest(NULL),
	fReadMutex(),
	fCurrentModule(0),
	fState(kReadingFirstRequest)
{
	this->SetTaskName("HTTPSession");

	//在全局服务对象中Session数增长一个,一个HTTPSession代表了一个连接
	QTSServerInterface::GetServer()->AlterCurrentServiceSessionCount(1);

	fModuleState.curModule = NULL;
	fModuleState.curTask = this;
	fModuleState.curRole = 0;
	fModuleState.globalLockRequested = false;

	//fDeviceSnap = NEW char[EASY_MAX_URL_LENGTH];
	//fDeviceSnap[0] = '\0';

	qtss_printf("create session:%s\n", fSessionID);
}

HTTPSession::~HTTPSession()
{
	fLiveSession = false; //used in Clean up request to remove the RTP session.
	this->CleanupRequest();// Make sure that all our objects are deleted
	//if (fSessionType == qtssServiceSession)
	//    QTSServerInterface::GetServer()->AlterCurrentServiceSessionCount(-1);

	//if (fDeviceSnap != NULL)
		//delete [] fDeviceSnap; 

}

/*!
\brief 事件由HTTPSession Task进行处理，大多数为网络报文处理事件 
\param 
\return 处理完成返回0,断开Session返回-1
\ingroup 
\see 
*/
SInt64 HTTPSession::Run()
{
	//获取事件类型
	EventFlags events = this->GetEvents();
	QTSS_Error err = QTSS_NoErr;
	QTSSModule* theModule = NULL;
	UInt32 numModules = 0;
	// Some callbacks look for this struct in the thread object
	OSThreadDataSetter theSetter(&fModuleState, NULL);

	//超时事件或者Kill事件，进入释放流程：清理 & 返回-1
	if (events & Task::kKillEvent)
		fLiveSession = false;

	//这部分应该也是返回false比较合理吧，因为当检测到超时时会向Session发送超时事件，如果粗暴的返回-1，则会进入delete环节直接将Session删除，那么如果当前Session被引用，在访问当前Session时就会直接崩溃。
	if(events & Task::kTimeoutEvent)
	{
		//客户端Session超时，暂时不处理 
		char msgStr[512];
		qtss_snprintf(msgStr, sizeof(msgStr), "session timeout,release session, device_serial[%s]\n", fDevice.serial_.c_str());
		QTSServerInterface::LogError(qtssMessageVerbosity, msgStr);
		//return -1;
		fLiveSession = false;
	}

	//正常事件处理流程
	while (this->IsLiveSession())
	{
		//报文处理以状态机的形式，可以方便多次处理同一个消息
		switch (fState)
		{
		case kReadingFirstRequest://首次对Socket进行读取
			{
				if ((err = fInputStream.ReadRequest()) == QTSS_NoErr)
				{
					//如果RequestStream返回QTSS_NoErr，就表示已经读取了目前所到达的网络数据
					//但，还不能构成一个整体报文，还要继续等待读取...
					fInputSocketP->RequestEvent(EV_RE);
					return 0;
				}

				if ((err != QTSS_RequestArrived) && (err != E2BIG))
				{
					// Any other error implies that the client has gone away. At this point,
					// we can't have 2 sockets, so we don't need to do the "half closed" check
					// we do below
					Assert(err > 0); 
					Assert(!this->IsLiveSession());
					break;
				}

				if ((err == QTSS_RequestArrived) || (err == E2BIG))
					fState = kHaveCompleteMessage;
			}
			continue;            
		case kReadingRequest://读取请求报文
			{
				//读取锁，已经在处理一个报文包时，不进行新网络报文的读取和处理
				OSMutexLocker readMutexLocker(&fReadMutex);

				//网络请求报文存储在fInputStream中
				if ((err = fInputStream.ReadRequest()) == QTSS_NoErr)
				{
					//如果RequestStream返回QTSS_NoErr，就表示已经读取了目前所到达的网络数据
					//但，还不能构成一个整体报文，还要继续等待读取...
					fInputSocketP->RequestEvent(EV_RE);
					return 0;
				}

				if ((err != QTSS_RequestArrived) && (err != E2BIG) && (err != QTSS_BadArgument))
				{
					//Any other error implies that the input connection has gone away.
					// We should only kill the whole session if we aren't doing HTTP.
					// (If we are doing HTTP, the POST connection can go away)
					Assert(err > 0);
					if (fOutputSocketP->IsConnected())
					{
						// If we've gotten here, this must be an HTTP session with
						// a dead input connection. If that's the case, we should
						// clean up immediately so as to not have an open socket
						// needlessly lingering around, taking up space.
						Assert(fOutputSocketP != fInputSocketP);
						Assert(!fInputSocketP->IsConnected());
						fInputSocketP->Cleanup();
						return 0;
					}
					else
					{
						Assert(!this->IsLiveSession());
						break;
					}
				}
				fState = kHaveCompleteMessage;
			}
		case kHaveCompleteMessage://读取到完整的请求报文
			{
				Assert( fInputStream.GetRequestBuffer() );

				Assert(fRequest == NULL);
				//根据具体请求报文构造HTTPRequest请求类
				fRequest = NEW HTTPRequest(&QTSServerInterface::GetServerHeader(), fInputStream.GetRequestBuffer());

				//在这里，我们已经读取了一个完整的Request，并准备进行请求的处理，直到响应报文发出
				//在此过程中，此Session的Socket不进行任何网络数据的读/写；
				fReadMutex.Lock();
				fSessionMutex.Lock();

				//清空发送缓冲区
				fOutputStream.ResetBytesWritten();

				//网络请求超过了缓冲区，返回Bad Request
				if (err == E2BIG)
				{
					//返回HTTP报文，错误码408
					//(void)QTSSModuleUtils::SendErrorResponse(fRequest, qtssClientBadRequest, qtssMsgRequestTooLong);
					fState = kSendingResponse;
					break;
				}
				// Check for a corrupt base64 error, return an error
				if (err == QTSS_BadArgument)
				{
					//返回HTTP报文，错误码408
					//(void)QTSSModuleUtils::SendErrorResponse(fRequest, qtssClientBadRequest, qtssMsgBadBase64);
					fState = kSendingResponse;
					break;
				}

				Assert(err == QTSS_RequestArrived);
				fState = kFilteringRequest;
			}

		case kFilteringRequest:
			{
				//刷新Session保活时间
				fTimeoutTask.RefreshTimeout();

				//对请求报文进行解析
				QTSS_Error theErr = SetupRequest();
				//当SetupRequest步骤未读取到完整的网络报文，需要进行等待
				if(theErr == QTSS_WouldBlock)
				{
					this->ForceSameThread();
					fInputSocketP->RequestEvent(EV_RE);
					// We are holding mutexes, so we need to force
					// the same thread to be used for next Run()
					return 0;//返回0表示有事件才进行通知，返回>0表示规定事件后调用Run

				}
				//应该再就加上theErr的判断！=QTSS_NOERR就应该直接断开连接或者返回错误码，否则下面不一定够得到有效的数据

				//每一步都检测响应报文是否已完成，完成则直接进行回复响应
				if (fOutputStream.GetBytesWritten() > 0)
				{
					fState = kSendingResponse;
					break;
				}

				fState = kPreprocessingRequest;
				break;
			}

		case kPreprocessingRequest:
			{
				//add
				ProcessRequest();//处理请求
				if (fOutputStream.GetBytesWritten() > 0)//每一步都检测响应报文是否已完成，完成则直接进行回复响应
				{
					delete[] fRequestBody;//释放数据部分
					fRequestBody=NULL;
					fState = kSendingResponse;
					break;
				}
				//走到这说明没有进行请求处理，应该是等待其他HTTPSession的回应
				if(fInfo.uWaitingTime>0)
				{
					this->ForceSameThread();
					// We are holding mutexes, so we need to force
					// the same thread to be used for next Run()
					UInt32 iTemp=fInfo.uWaitingTime;
					fInfo.uWaitingTime=0;//下次等待时间需要重新被赋值
					return iTemp;//等待下一个周期被调用
				}
				delete[] fRequestBody;//释放数据部分,注意在此可能指针为空
				fRequestBody=NULL;
				fState = kCleaningUp;
				break;
			}

		case kProcessingRequest:
			{
				if (fOutputStream.GetBytesWritten() == 0)
				{
					// 响应报文还没有形成
					//QTSSModuleUtils::SendErrorResponse(fRequest, qtssServerInternal, qtssMsgNoModuleForRequest);
					fState = kCleaningUp;
					break;
				}

				fState = kSendingResponse;
			}
		case kSendingResponse:
			{
				//响应报文发送，确保完全发送
				Assert(fRequest != NULL);

				//发送响应报文
				err = fOutputStream.Flush();

				if (err == EAGAIN)
				{
					// If we get this error, we are currently flow-controlled and should
					// wait for the socket to become writeable again
					//如果收到Socket EAGAIN错误，那么我们需要等Socket再次可写的时候再调用发送
					fSocket.RequestEvent(EV_WR);
					this->ForceSameThread();
					// We are holding mutexes, so we need to force
					// the same thread to be used for next Run()
					return 0;
				}
				else if (err != QTSS_NoErr)
				{
					// Any other error means that the client has disconnected, right?
					Assert(!this->IsLiveSession());
					break;
				}

				fState = kCleaningUp;
			}

		case kCleaningUp:
			{
				// Cleaning up consists of making sure we've read all the incoming Request Body
				// data off of the socket
				if (this->GetRemainingReqBodyLen() > 0)
				{
					err = this->DumpRequestData();

					if (err == EAGAIN)
					{
						fInputSocketP->RequestEvent(EV_RE);
						this->ForceSameThread();    // We are holding mutexes, so we need to force
						// the same thread to be used for next Run()
						return 0;
					}
				}

				//一次请求的读取、处理、响应过程完整，等待下一次网络报文！
				this->CleanupRequest();
				fState = kReadingRequest;
			}
		}
	} 

	//清空Session占用的所有资源
	this->CleanupRequest();

	//Session引用数为0，返回-1后，系统会将此Session删除
	if (fObjectHolders == 0)
		return -1;

	//如果流程走到这里，Session实际已经无效了，应该被删除，但没有，因为还有其他地方引用了Session对象
	return 0;
}

/*
发送HTTP+json报文，决定是否关闭当前Session
HTTP部分构造，json部分由函数传递
*/
QTSS_Error HTTPSession::SendHTTPPacket(StrPtrLen* contentXML, Bool16 connectionClose, Bool16 decrement)
{
	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(contentXML->Len?httpOK:httpNotImplemented);
	if (contentXML->Len)
		httpAck.AppendContentLengthHeader(contentXML->Len);

	if(connectionClose)
		httpAck.AppendConnectionCloseHeader();

	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (contentXML->Len > 0) 
		pOutputStream->Put(contentXML->Ptr, contentXML->Len);

	if (pOutputStream->GetBytesWritten() != 0)
	{
		pOutputStream->Flush();
	}

	//将对HTTPSession的引用减少一
	if(fObjectHolders && decrement)
		DecrementObjectHolderCount();

	if(connectionClose)
		this->Signal(Task::kKillEvent);

	return QTSS_NoErr;
}

/*
	Content报文读取与解析同步进行报文处理，构造回复报文
*/
QTSS_Error HTTPSession::SetupRequest()
{
	//解析请求报文
	QTSS_Error theErr = fRequest->Parse();
	if (theErr != QTSS_NoErr)
		return QTSS_BadArgument;

	if (fRequest->GetRequestPath() != NULL)
	{
		string sRequest(fRequest->GetRequestPath());
		//数据是放在Json部分还是放在HTTP头部分
		//1.数据放在头部则是 post http://****/123,则123为数据部分，或者使用post /123则123为数据部分。
		//2.数据放在JSON部分，则是post http://****/,头部不包含数据部分，跳过进入JSON部分的处理。
		if (!sRequest.empty())
		{
			boost::to_lower(sRequest);

			vector<string> path;
			if (boost::ends_with(sRequest, "/"))
			{
				boost::erase_tail(sRequest, 1);
			}
			boost::split(path, sRequest, boost::is_any_of("/"), boost::token_compress_on);
			if (path.size() == 2)
			{
				if(path[0]=="api"&&path[1]=="getdevicelist")
				{
					ExecNetMsgGetDeviceListReqEx(fRequest->GetQueryString());//获得设备列表
					return 0;
				}
				else if(path[0]=="api"&&path[1]=="getdeviceinfo")
				{
					ExecNetMsgGetCameraListReqEx(fRequest->GetQueryString());//获得某个设备详细信息
					return 0;
				}
				else if(path[0]=="api"&&path[1]=="getdevicestream")
				{
					ExecNetMsgStreamStartReqRestful(fRequest->GetQueryString());//客户端的直播请求
					return 0;
				}
				else if(path[0]=="api"&&path[1]=="freedevicestream")
				{
					ExecNetMsgStreamStopReqRestful(fRequest->GetQueryString());//客户端的停止直播请求
					return 0;
				}
			}

			// 执行到这的都说明需要进行异常处理
			EasyMsgExceptionACK rsp;
			string msg = rsp.GetMsg();
			// 构造响应报文(HTTP Header)
			HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
			httpAck.CreateResponseHeader(!msg.empty() ? httpOK : httpNotImplemented);
			if (!msg.empty())
				httpAck.AppendContentLengthHeader((UInt32)msg.length());

			// 判断是否需要关闭当前Session连接
			//if(connectionClose)
			httpAck.AppendConnectionCloseHeader();

			//Push HTTP Header to OutputBuffer
			char respHeader[2048] = { 0 };
			StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
			strncpy(respHeader, ackPtr->Ptr, ackPtr->Len);

			HTTPResponseStream *pOutputStream = GetOutputStream();
			pOutputStream->Put(respHeader);

			//Push HTTP Content to OutputBuffer
			if (!msg.empty())
				pOutputStream->Put((char*)msg.data(), msg.length());

			return QTSS_NoErr;
		}		
	}

	HTTPStatusCode statusCode = httpOK;
	char *body = NULL;
	UInt32 bodySizeBytes = 0;

	//获取具体Content json数据部分

	//1、获取json部分长度
	StrPtrLen* lengthPtr = fRequest->GetHeaderValue(httpContentLengthHeader);

	StringParser theContentLenParser(lengthPtr);
	theContentLenParser.ConsumeWhitespace();
	UInt32 content_length = theContentLenParser.ConsumeInteger(NULL);

	qtss_printf("HTTPSession read content-length:%d \n", content_length);

	if (content_length <= 0)
	{
		return QTSS_BadArgument;
	}

	// Check for the existence of 2 attributes in the request: a pointer to our buffer for
	// the request body, and the current offset in that buffer. If these attributes exist,
	// then we've already been here for this request. If they don't exist, add them.
	UInt32 theBufferOffset = 0;
	char* theRequestBody = NULL;
	UInt32 theLen = 0;
	theLen = sizeof(theRequestBody);
	theErr = QTSS_GetValue(this, qtssEasySesContentBody, 0, &theRequestBody, &theLen);

	if (theErr != QTSS_NoErr)
	{
		// First time we've been here for this request. Create a buffer for the content body and
		// shove it in the request.
		theRequestBody = NEW char[content_length + 1];
		memset(theRequestBody,0,content_length + 1);
		theLen = sizeof(theRequestBody);
		theErr = QTSS_SetValue(this, qtssEasySesContentBody, 0, &theRequestBody, theLen);// SetValue creates an internal copy.
		Assert(theErr == QTSS_NoErr);

		// Also store the offset in the buffer
		theLen = sizeof(theBufferOffset);
		theErr = QTSS_SetValue(this, qtssEasySesContentBodyOffset, 0, &theBufferOffset, theLen);
		Assert(theErr == QTSS_NoErr);
	}

	theLen = sizeof(theBufferOffset);
	theErr = QTSS_GetValue(this, qtssEasySesContentBodyOffset, 0, &theBufferOffset, &theLen);

	// We have our buffer and offset. Read the data.
	//theErr = QTSS_Read(this, theRequestBody + theBufferOffset, content_length - theBufferOffset, &theLen);
	theErr = fInputStream.Read(theRequestBody + theBufferOffset, content_length - theBufferOffset, &theLen);
	Assert(theErr != QTSS_BadArgument);

	if (theErr == QTSS_RequestFailed)
	{
		OSCharArrayDeleter charArrayPathDeleter(theRequestBody);
		//
		// NEED TO RETURN HTTP ERROR RESPONSE
		return QTSS_RequestFailed;
	}

	qtss_printf("Add Len:%d \n", theLen);
	qtss_printf("HTTPSession read content-length:%d (%d/%d) \n", theLen, theBufferOffset+theLen, content_length);
	if ((theErr == QTSS_WouldBlock) || (theLen < ( content_length - theBufferOffset)))
	{
		//
		// Update our offset in the buffer
		theBufferOffset += theLen;
		(void)QTSS_SetValue(this, qtssEasySesContentBodyOffset, 0, &theBufferOffset, sizeof(theBufferOffset));
		// The entire content body hasn't arrived yet. Request a read event and wait for it.

		Assert(theErr == QTSS_NoErr);
		return QTSS_WouldBlock;
	}
	//执行到这说明已经接收了完整的HTTPhead+JSON部分
	fRequestBody=theRequestBody;//将数据部分保存起来，让ProcessRequest函数去处理请求。
	Assert(theErr == QTSS_NoErr);
	qtss_printf("Recv message: %s\n", fRequestBody);


	UInt32 offset = 0;
	(void)QTSS_SetValue(this, qtssEasySesContentBodyOffset, 0, &offset, sizeof(offset));
	char* content = NULL;
	(void)QTSS_SetValue(this, qtssEasySesContentBody, 0, &content, 0);

	return QTSS_NoErr;
}

void HTTPSession::CleanupRequest()
{
	if (fRequest != NULL)
	{
		// NULL out any references to the current request
		delete fRequest;
		fRequest = NULL;
	}

	fSessionMutex.Unlock();
	fReadMutex.Unlock();

	// Clear out our last value for request body length before moving onto the next request
	this->SetRequestBodyLength(-1);
}

Bool16 HTTPSession::OverMaxConnections(UInt32 buffer)
{
	QTSServerInterface* theServer = QTSServerInterface::GetServer();
	SInt32 maxConns = theServer->GetPrefs()->GetMaxConnections();
	Bool16 overLimit = false;

	if (maxConns > -1) // limit connections
	{ 
		UInt32 maxConnections = (UInt32) maxConns + buffer;
		if  ( theServer->GetNumServiceSessions() > maxConnections ) 
		{
			overLimit = true;          
		}
	}
	return overLimit;
}


QTSS_Error HTTPSession::DumpRequestData()
{
	char theDumpBuffer[EASY_REQUEST_BUFFER_SIZE_LEN];

	QTSS_Error theErr = QTSS_NoErr;
	while (theErr == QTSS_NoErr)
		theErr = this->Read(theDumpBuffer, EASY_REQUEST_BUFFER_SIZE_LEN, NULL);

	return theErr;
}

// MSG_DS_REGISTER_REQ消息处理
QTSS_Error HTTPSession::ExecNetMsgDevRegisterReq(const char* json)
{	
	QTSS_Error theErr = QTSS_NoErr;		

	EasyDarwin::Protocol::EasyMsgDSRegisterREQ req(json);
	do
	{
		// 1、需要先确认Session是否已经验证过，如果未验证，先要进行验证
		//if(fAuthenticated)
		//{
		//	break;
		//}

		// 2、这里需要对TerminalType和AppType做判断，是否为EasyCamera和EasyNVR
		fSessionType = qtssDeviceSession;

		boost::to_lower(req.GetNVR().serial_);
		fDevSerial = req.GetNVR().serial_;
		EasyNVRs &nvrs = QTSServerInterface::GetServer()->GetRegisterNVRs();
		req.GetNVR().object_ = this;
		if(nvrs.find(fDevSerial) != nvrs.end())
		{
			//更新信息
			theErr =  QTSS_AttrNameExists;
			nvrs[fDevSerial] = req.GetNVR();			
		}
		else
		{
			//认证授权标识,当前Session就不需要再进行认证过程了
			fAuthenticated = true;			
			nvrs.insert(make_pair(fDevSerial, req.GetNVR()));
		}

	}while(0);

	if(theErr != QTSS_NoErr) return theErr;


	EasyJsonValue body;
	body["Serial"] = fDevSerial;
	body["SessionID"] = fSessionID;

	EasyDarwin::Protocol::EasyMsgSDRegisterACK rsp(body, 1, 200);

	string msg = rsp.GetMsg();

	//StrPtrLen jsonRSP((char*)msg.c_str());

	//构造响应报文(HTTP Header)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//判断是否需要关闭当前Session连接
	//if(connectionClose)
	//	httpAck.AppendConnectionCloseHeader();

	//Push HTTP Header to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);

	//Push HTTP Content to OutputBuffer
	if (!msg.empty()) 
		pOutputStream->Put((char*)msg.data(), msg.length());

	return QTSS_NoErr;
}

QTSS_Error HTTPSession::ExecNetMsgGetDeviceListReq(char *queryString)
{
	QTSS_Error theErr = QTSS_NoErr;

	qtss_printf("Get Device List! \n");

	QueryParamList parList(queryString);	
	const char* tag_name = parList.DoFindCGIValueForParam("tagname");

	EasyNVRs &nvrs = QTSServerInterface::GetServer()->GetRegisterNVRs();

	EasyDevices devices;

	for (EasyNVRs::iterator it = nvrs.begin(); it != nvrs.end(); it++)
	{		
		do
		{
			EasyDevice device;
			device.name_ = it->second.name_;
			device.serial_ = it->second.serial_;
			device.tag_ = it->second.tag_;
			if(tag_name != NULL && it->second.tag_ != tag_name)
			{
				break;
			}
			devices.push_back(device);
		}while (0);		
	}

	EasyMsgSCDeviceListACK rsp(devices);

	string msg = rsp.GetMsg();

	qtss_printf(msg.c_str());

	//StrPtrLen msgJson((char*)msg.c_str());

	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//响应完成后断开连接
	httpAck.AppendConnectionCloseHeader();

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (!msg.empty())
		pOutputStream->Put((char*)msg.data(), msg.length());

	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgGetCameraListReq(const string& device_serial, char* queryString)
{
	QTSS_Error theErr = QTSS_NoErr;
	string msg;
	do
	{
		QueryParamList parList(queryString);
		EasyDevices cameras;

		/*const char* device_serial = parList.DoFindCGIValueForParam("device");
		if (device_serial == NULL)
		{
		theErr = QTSS_ValueNotFound;
		EasyMsgSCDeviceInfoACK rsp(cameras, "", 1, EASY_ERROR_CLIENT_BAD_REQUEST);
		msg = rsp.GetMsg();
		qtss_printf("Get Camera List error: Not found device serial arg! \n", queryString);
		break;
		}*/

		qtss_printf("Get Camera List for device[%s]! \n", device_serial.c_str());

		EasyNVRs &nvrs = QTSServerInterface::GetServer()->GetRegisterNVRs();
		EasyNVRs::iterator nvr = nvrs.find(device_serial);

		if (nvr == nvrs.end())
		{
			theErr = QTSS_AttrDoesntExist;			
			EasyMsgSCDeviceInfoACK rsp(cameras, device_serial, 1, EASY_ERROR_DEVICE_NOT_FOUND);
			msg = rsp.GetMsg();
		}
		else
		{
			EasyMsgSCDeviceInfoACK rsp(nvr->second.channels_, device_serial);
			msg = rsp.GetMsg();
		}
		qtss_printf(msg.c_str());
	} while (0);
	//StrPtrLen msgJson((char*)msg.c_str());

	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty() ? httpOK : httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//响应完成后断开连接
	httpAck.AppendConnectionCloseHeader();

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader, ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (!msg.empty())
		pOutputStream->Put((char*)msg.data(), msg.length());

	return theErr;
}
QTSS_Error HTTPSession::ExecNetMsgStartStreamReq(const string& device_serial, char * queryString)
{
	QTSS_Error theErr = QTSS_NoErr;
	string msg;
	do
	{
		QueryParamList parList(queryString);
		EasyJsonValue body;

		//const char* device_serial = parList.DoFindCGIValueForParam("device");
		const char* camera_serial = parList.DoFindCGIValueForParam("camera");
		const char* protocol = parList.DoFindCGIValueForParam("protocol");
		const char* stream_id = parList.DoFindCGIValueForParam("streamid");

		if (/*device_serial == NULL || */camera_serial == NULL || protocol == NULL || stream_id == NULL)
		{
			theErr = QTSS_BadArgument;

			EasyMsgSCGetStreamACK rsp(body, 1, EASY_ERROR_CLIENT_BAD_REQUEST);
			msg = rsp.GetMsg();
			qtss_printf("client start stream error: bad argument[%s]! \n", queryString);
			break;
		}

		qtss_printf("client start stream [%s]! \n", queryString);

		EasyNVRs &nvrs = QTSServerInterface::GetServer()->GetRegisterNVRs();
		EasyNVRs::iterator nvr = nvrs.find(device_serial);

		if (nvr == nvrs.end())
		{
			theErr = QTSS_AttrDoesntExist;
			EasyMsgSCGetStreamACK rsp(body, 1, EASY_ERROR_DEVICE_NOT_FOUND);
			msg = rsp.GetMsg();
		}
		else
		{			
			string dss_ip = QTSServerInterface::GetServer()->GetPrefs()->GetDssIP();
			UInt16 dss_port = QTSServerInterface::GetServer()->GetPrefs()->GetDssPort();

			HTTPSession* nvr_session = (HTTPSession*)nvr->second.object_;

			if (nvr_session->GetStreamReqCount(camera_serial) == 0)
			{
				//start device stream						
				body["DeviceSerial"] = device_serial;
				body["CameraSerial"] = camera_serial;
				body["StreamID"] = stream_id;
				body["Protocol"] = "RTSP";

				body["DssIP"] = dss_ip;
				body["DssPort"] = dss_port;

				EasyMsgSDPushStreamREQ req(body, 1);
				string buffer = req.GetMsg();

				nvr_session->SetStreamPushInfo(body);

				EasyNVRMessage nvr_msg;
				nvr_msg.device_serial_ = device_serial;
				nvr_msg.camera_serial_ = camera_serial;
				nvr_msg.stream_id_ = stream_id;
				nvr_msg.message_type_ = MSG_CS_GET_STREAM_REQ;
				nvr_msg.object_ = this;
				nvr_msg.timeout_ = sWaitDeviceRspTimeout;
				nvr_msg.timestamp_ = EasyUtil::NowTime();

				nvr_session->PushNVRMessage(nvr_msg);

				QTSS_SendHTTPPacket(nvr->second.object_, (char*)buffer.c_str(), buffer.length(), false, false);

				//TODO:: wait for device response	
				boost::unique_lock<boost::mutex> lock(nvr_session->fNVROperatorMutex);
				if (!fCond.timed_wait(lock, boost::get_system_time() + boost::posix_time::seconds(sWaitDeviceRspTimeout)))
				{
					theErr = QTSS_RequestFailed;
					body.clear();
					EasyMsgSCGetStreamACK rsp(body, 1, EASY_ERROR_REQUEST_TIMEOUT);
					msg = rsp.GetMsg();
					break;
				}				
			}
			else
			{
				try
				{
					dss_ip = boost::apply_visitor(EasyJsonValueVisitor(), nvr_session->GetStreamPushInfo()["DssIP"]);
					dss_port = EasyUtil::String2Int(boost::apply_visitor(EasyJsonValueVisitor(), nvr_session->GetStreamPushInfo()["DssPort"]));
				}
				catch (std::exception &e)
				{
					qtss_printf("HTTPSession::ExecNetMsgStartStreamReq get stream push info error: %s\n", e.what());
				}
			}

			if (dss_port != 554)
			{
				dss_ip += ":" + EasyUtil::Int2String(dss_port);
			}

			nvr_session->IncrementStreamReqCount(camera_serial);
			body.clear();
			body["PlayCount"] = (int)nvr_session->GetStreamReqCount(camera_serial);
			body["DeviceSerial"] = device_serial;
			body["CameraSerial"] = camera_serial;
			//TODO:: setup url
			body["URL"] = "rtsp://" + dss_ip + "/" + device_serial + "_" + camera_serial + ".sdp";
			body["Protocol"] = protocol;
			body["StreamID"] = stream_id;
			EasyMsgSCGetStreamACK rsp(body);
			msg = rsp.GetMsg();			

		}
		qtss_printf(msg.c_str());
	} while (0);
	//StrPtrLen msgJson((char*)msg.c_str());

	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty() ? httpOK : httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//响应完成后断开连接
	httpAck.AppendConnectionCloseHeader();

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader, ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (!msg.empty())
		pOutputStream->Put((char*)msg.data(), msg.length());

	return theErr;
}
QTSS_Error HTTPSession::ExecNetMsgStartDeviceStreamRsp(const char * json)
{
	QTSS_Error theErr = QTSS_NoErr;

	//qtss_printf("%s", json);

	EasyMsgDSPushSteamACK rsp(json);

	for (EasyNVRMessageQueue::iterator it = fNVRMessageQueue.begin(); it != fNVRMessageQueue.end();)
	{
		if (rsp.GetBodyValue("DeviceSerial") == it->device_serial_ &&
			rsp.GetBodyValue("CameraSerial") == it->camera_serial_ &&
			rsp.GetBodyValue("StreamID") == it->stream_id_ &&
			it->message_type_ == MSG_CS_GET_STREAM_REQ)
		{			
			if (EasyUtil::NowTime() - it->timestamp_ > it->timeout_)
			{				
				theErr = QTSS_RequestFailed;
			}
			else
			{
				HTTPSession* client = (HTTPSession*)it->object_;
				boost::mutex::scoped_lock lock(fNVROperatorMutex);				
				client->fCond.notify_one();				
			}
			it = fNVRMessageQueue.erase(it);
		}
		else
		{
			it++;
		}
	}
	return theErr;
}
QTSS_Error HTTPSession::ExecNetMsgStopStreamReq(const string& device_serial, char * queryString)
{
	QTSS_Error theErr = QTSS_NoErr;
	string msg;
	do
	{
		QueryParamList parList(queryString);
		EasyJsonValue body;

		//const char* device_serial = parList.DoFindCGIValueForParam("device");
		const char* camera_serial = parList.DoFindCGIValueForParam("camera");
		const char* protocol = parList.DoFindCGIValueForParam("protocol");
		const char* stream_id = parList.DoFindCGIValueForParam("streamid");

		if (/*device_serial == NULL || */camera_serial == NULL || protocol == NULL || stream_id == NULL)
		{
			theErr = QTSS_BadArgument;

			EasyMsgSCFreeStreamACK rsp(body, 1, EASY_ERROR_CLIENT_BAD_REQUEST);
			msg = rsp.GetMsg();
			qtss_printf("client start stream error: bad argument[%s]! \n", queryString);
			break;
		}

		qtss_printf("client start stream [%s]! \n", queryString);

		EasyNVRs &nvrs = QTSServerInterface::GetServer()->GetRegisterNVRs();
		EasyNVRs::iterator nvr = nvrs.find(device_serial);

		if (nvr == nvrs.end())
		{
			theErr = QTSS_AttrDoesntExist;
			EasyMsgSCFreeStreamACK rsp(body, 1, EASY_ERROR_DEVICE_NOT_FOUND);
			msg = rsp.GetMsg();
		}
		else
		{
			HTTPSession *nvr_session = (HTTPSession*)nvr->second.object_;

			if (nvr_session->GetStreamReqCount(camera_serial) == 1)
			{
				//stop device stream						
				body["DeviceSerial"] = device_serial;
				body["CameraSerial"] = camera_serial;
				body["StreamID"] = stream_id;

				EasyMsgSDStopStreamREQ req(body, 1);
				string buffer = req.GetMsg();

				EasyNVRMessage nvr_msg;
				nvr_msg.device_serial_ = device_serial;
				nvr_msg.camera_serial_ = camera_serial;
				nvr_msg.stream_id_ = stream_id;
				nvr_msg.message_type_ = MSG_CS_FREE_STREAM_REQ;
				nvr_msg.object_ = this;
				nvr_msg.timeout_ = sWaitDeviceRspTimeout;
				nvr_msg.timestamp_ = EasyUtil::NowTime();
				nvr_session->PushNVRMessage(nvr_msg);

				QTSS_SendHTTPPacket(nvr_session, (char*)buffer.c_str(), buffer.length(), false, false);	

				body.clear();
				//TODO:: wait for device response	
				boost::unique_lock<boost::mutex> lock(nvr_session->fNVROperatorMutex);
				if (!fCond.timed_wait(lock, boost::get_system_time() + boost::posix_time::seconds(sWaitDeviceRspTimeout)))
				{
					theErr = QTSS_RequestFailed;
					body["PlayCount"] = (int)nvr_session->GetStreamReqCount(camera_serial);
					EasyMsgSCFreeStreamACK rsp(body, 1, EASY_ERROR_REQUEST_TIMEOUT);
					msg = rsp.GetMsg();
					break;
				}

				nvr_session->DecrementStreamReqCount(camera_serial);
				body["PlayCount"] = (int)nvr_session->GetStreamReqCount(camera_serial);
				body["DeviceSerial"] = device_serial;
				body["CameraSerial"] = camera_serial;
				body["Protocol"] = protocol;
				body["StreamID"] = stream_id;
				EasyMsgSCFreeStreamACK rsp(body);
				msg = rsp.GetMsg();
			}
			else
			{
				nvr_session->DecrementStreamReqCount(camera_serial);
				body["PlayCount"] = (int)nvr_session->GetStreamReqCount(camera_serial);
				EasyMsgSCFreeStreamACK rsp(body, 1, EASY_ERROR_CONFLICT);
				msg = rsp.GetMsg();				
			}

		}
		qtss_printf(msg.c_str());
	} while (0);
	//StrPtrLen msgJson((char*)msg.c_str());

	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty() ? httpOK : httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//响应完成后断开连接
	httpAck.AppendConnectionCloseHeader();

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader, ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (!msg.empty())
		pOutputStream->Put((char*)msg.data(), msg.length());

	return theErr;
}
QTSS_Error HTTPSession::ExecNetMsgStopDeviceStreamRsp(const char * json)
{
	QTSS_Error theErr = QTSS_NoErr;

	//qtss_printf("%s", json);

	EasyMsgSDStopStreamREQ rsp(json);

	for (EasyNVRMessageQueue::iterator it = fNVRMessageQueue.begin(); it != fNVRMessageQueue.end();)
	{
		if (rsp.GetBodyValue("DeviceSerial") == it->device_serial_ &&
			rsp.GetBodyValue("CameraSerial") == it->camera_serial_ &&
			rsp.GetBodyValue("StreamID") == it->stream_id_ &&
			it->message_type_ == MSG_CS_FREE_STREAM_REQ)
		{
			if (EasyUtil::NowTime() - it->timestamp_ > it->timeout_)
			{				
				theErr = QTSS_RequestFailed;
			}
			else
			{
				HTTPSession* client = (HTTPSession*)it->object_;
				boost::mutex::scoped_lock lock(fNVROperatorMutex);
				client->fCond.notify_one();				
			}
			it = fNVRMessageQueue.erase(it);			
		}
		else
		{
			it++;
		}
	}

	return theErr;
}
//保留，end

//公共，begin
QTSS_Error HTTPSession::ExecNetMsgSnapUpdateReq(const char* json)//设备快照请求
{
	if(!fAuthenticated) return httpUnAuthorized;

	EasyDarwin::Protocol::EasyMsgDSPostSnapREQ parse(json);

	string image			=	parse.GetBodyValue("Image");	
	string camer_serial		=	parse.GetBodyValue("Channel");
	string device_serial	=	parse.GetBodyValue("Serial");
	string strType			=	parse.GetBodyValue("Type");//类型就是图片的扩展名
	string strTime			=	parse.GetBodyValue("Time");//时间属性

	if(camer_serial.empty())//为可选项填充默认值
		camer_serial="01";
	if(strTime.empty())//如果没有时间属性，则服务端自动为其生成一个
		strTime=EasyUtil::NowTime(EASY_TIME_FORMAT_YYYYMMDDHHMMSS);
		
	if(image.size()<=0||device_serial.size()<=0||strType.size()<=0||strTime.size()<=0)
		return QTSS_BadArgument;

	//先对数据进行Base64解码
	image = EasyUtil::Base64Decode(image.data(), image.size());

	//文件夹路径，由快照路径+Serial合成
	char jpgDir[512] = { 0 };
	qtss_sprintf(jpgDir,"%s%s", QTSServerInterface::GetServer()->GetPrefs()->GetSnapLocalPath() ,device_serial.c_str());
	OS::RecursiveMakeDir(jpgDir);

	char jpgPath[512] = { 0 };

	//文件全路径，文件名由Serial_Channel_Time.Type合成
	qtss_sprintf(jpgPath,"%s/%s_%s_%s.%s", jpgDir, device_serial.c_str(), camer_serial.c_str(), "t"/*strTime.c_str()*/,strType.c_str());

	//保存快照数据
	FILE* fSnap = ::fopen(jpgPath, "wb");
	fwrite(image.data(), 1, image.size(), fSnap);
	::fclose(fSnap);

	//设备快照需要保留多个时间属性，一个摄像头一个
	fDevice.HoldSnapPath(jpgPath,camer_serial);

	//qtss_sprintf(fDeviceSnap, "%s/%s/%s_%s.%s",QTSServerInterface::GetServer()->GetPrefs()->GetSnapWebPath(), device_serial.c_str(), device_serial.c_str(),camer_serial.c_str(),strType.c_str());

	EasyDarwinRSP rsp(MSG_SD_POST_SNAP_ACK);
	EasyJsonValue header,body;

	header[EASY_TAG_VERSION]=EASY_PROTOCOL_VERSION;
	header["CSeq"]=parse.GetHeaderValue("CSeq");
	header["ErrorNum"]=200;
	header["ErrorString"]=EasyProtocol::GetErrorString(200);

	body["Serial"] = fDevSerial.c_str();
	body["Channel"] = camer_serial.c_str();

	string msg = rsp.GetMsg();

	//QTSS_SendHTTPPacket(this, (char*)msg.c_str(), msg.length(), false, false);
	//构造响应报文(HTTP Header)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);

	//将相应报文添加到HTTP输出缓冲区中
	if (!msg.empty()) 
		pOutputStream->Put((char*)msg.data(), msg.length());
	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgDefaultReqHandler(const char* json)
{
	return EASY_ERROR_SERVER_NOT_IMPLEMENTED;//add
	//return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgDevRegisterReqEx(const char* json)//设备注册认证请求，其他请求处理前一定要经过认证
{
	QTSS_Error theErr = QTSS_NoErr;		
	do
	{
		if(fAuthenticated)//如果已经认证，则不对信息进行处理
		{
			//如果有什么信息是认证之后还要处理的，则需要在这和外面分别进行处理
			break;
		}
		if(!fDevice.GetDevInfo(json))//获取设备信息失败
		{
			theErr=QTSS_BadArgument;
			break;
		}
		if(false)//验证设备的合法性
		{
			theErr=httpUnAuthorized;
			break;
		}
		fSessionType = qtssDeviceSession;//更新Session类型
		theErr = QTSServerInterface::GetServer()->GetDeviceMap()->Register(fDevice.serial_,this);
		if(theErr == OS_NoErr)
		{
			//认证授权标识,当前Session就不需要再进行认证过程了
			fAuthenticated = true;

			//在redis上增加设备
			QTSServerInterface::GetServer()->RedisAddDevName(fDevice.serial_.c_str());
		}
		else
		{
			//上线冲突
			theErr =  QTSS_AttrNameExists;
			break;
		}
	}while(0);

	if(theErr != QTSS_NoErr) return theErr;
	//走到这说明该设备成功注册或者心跳
	EasyProtocol req(json);
	EasyDarwinRSP rsp(MSG_SD_REGISTER_ACK);
	EasyJsonValue header,body;
	header[EASY_TAG_VERSION]=EASY_PROTOCOL_VERSION;
	header["CSeq"]=req.GetHeaderValue("CSeq");
	header["ErrorNum"]=200;
	header["ErrorString"]=EasyProtocol::GetErrorString(200);

	body["Serial"]=fDevice.serial_;
	body["SessionID"]=fSessionID;

	rsp.SetHead(header);
	rsp.SetBody(body);
	string msg = rsp.GetMsg();

	//构造响应报文(HTTP Header)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);

	//将相应报文添加到HTTP输出缓冲区中
	if (!msg.empty()) 
		pOutputStream->Put((char*)msg.data(), msg.length());

	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgStreamStopReqRestful(char *queryString)//客户端的停止直播请求，Restful接口，
//虽然可以在里面进行处理，但还是放到ExecNetMsgStreamStopReq中,只保留一份处理
{
		/*//暂时注释掉，实际上是需要认证的
	if(!fAuthenticated)//没有进行认证请求
	return httpUnAuthorized;
	*/
	QueryParamList parList(queryString);
	const char* chSerial	=	parList.DoFindCGIValueForParam("device");//获取设备序列号
	const char* chChannel	=	parList.DoFindCGIValueForParam("channel");//获取通道
	const char* chProtocol  =   parList.DoFindCGIValueForParam("protocol");//获取通道
	const char* chReserve	=	parList.DoFindCGIValueForParam("reserve");//获取通道

	
	//为可选参数填充默认值
	if(chChannel==NULL)
		chChannel="01";
	if(chReserve==NULL)
		chReserve="1";

	if(chSerial==NULL||chProtocol==NULL)
		return QTSS_BadArgument;

	EasyDarwin::Protocol::EasyDarwinRSP req(MSG_CS_FREE_STREAM_REQ);//由restful接口合成json格式请求
	EasyJsonValue header,body;

	char chTemp[16]={0};//如果客户端不提供CSeq,那么我们每次给他生成一个唯一的CSeq
	UInt32 uCseq=GetCSeq();
	sprintf(chTemp,"%d",uCseq);

	header["CSeq"]		=		chTemp;
	header["Version"]	=		"1.0";
	body["Serial"]		=		chSerial;
	body["Channel"]		=		chChannel;
	body["Protocol"]	=		chProtocol;
	body["Reserve"]		=		chReserve;

	req.SetHead(header);
	req.SetBody(body);

	string buffer=req.GetMsg();
	fRequestBody =new char[buffer.size()+1];
	strcpy(fRequestBody,buffer.c_str());
	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgStreamStopReq(const char* json)//客户端的停止直播请求
{
	//算法描述：查找指定设备，若设备存在，删除set中的当前客户端元素并判断set是否为空，为空则向设备发出停止流请求
	/*//暂时注释掉，实际上是需要认证的
	if(!fAuthenticated)//没有进行认证请求
	return httpUnAuthorized;
	*/

	EasyDarwin::Protocol::EasyProtocol req(json);
	string strCSeq=req.GetHeaderValue("CSeq");
	UInt32 uCSeq=atoi(strCSeq.c_str());

	string strDeviceSerial	=	req.GetBodyValue("Serial");//设备序列号
	string strCameraSerial	=	req.GetBodyValue("Channel");//摄像头序列号
	string strStreamID		=   req.GetBodyValue("Reserve");//StreamID
	string strProtocol		=	req.GetBodyValue("Protocol");//Protocol

	if(strDeviceSerial.size()<=0||strCameraSerial.size()<=0||strStreamID.size()<=0||strProtocol.size()<=0)//参数判断
		return QTSS_BadArgument;

	OSRefTableEx* DeviceMap=QTSServerInterface::GetServer()->GetDeviceMap();
	OSRefTableEx::OSRefEx* theDevRef=DeviceMap->Resolve(strDeviceSerial);////////////////////////////////++
	if(theDevRef==NULL)//找不到指定设备
		return EASY_ERROR_DEVICE_NOT_FOUND;

	HTTPSession * pDevSession=(HTTPSession *)theDevRef->GetObjectPtr();//获得当前设备回话
	stStreamInfo  stTemp;
	if(!FindInStreamMap(strDeviceSerial+strCameraSerial,stTemp))//没有对当前设备进行直播,有的话则删除对该摄像头的记录
	{
		DeviceMap->Release(strDeviceSerial);//////////////////////////////////////////////////////////--
		return QTSS_BadArgument;
	} 
	if(pDevSession->EraseInSet(strCameraSerial,this))//如果设备的客户端列表为空，则向设备发出停止推流请求
	{
		EasyDarwin::Protocol::EasyDarwinRSP		reqreq(MSG_SD_STREAM_STOP_REQ);
		EasyJsonValue headerheader,bodybody;

		char chTemp[16]={0};
		UInt32 uDevCseq=pDevSession->GetCSeq();
		sprintf(chTemp,"%d",uDevCseq);
		headerheader["CSeq"]	=string(chTemp);//注意这个地方不能直接将UINT32->int,因为会造成数据失真
		headerheader[EASY_TAG_VERSION]=		EASY_PROTOCOL_VERSION;

		bodybody["Serial"]		=	strDeviceSerial;
		bodybody["Channel"]		=	strCameraSerial;
		bodybody["Reserve"]		=   strStreamID;
		bodybody["Protocol"]	=	strProtocol;

		reqreq.SetHead(headerheader);
		reqreq.SetBody(bodybody);

		string buffer = reqreq.GetMsg();
		QTSS_SendHTTPPacket(pDevSession,(char*)buffer.c_str(),buffer.size(),false,false);
	}
	else//说明仍然有其他客户端正在对当前摄像头进行直播
	{
		
	}
	DeviceMap->Release(strDeviceSerial);//////////////////////////////////////////////////////////--
	//客户端并不关心对于停止直播的回应，因此对客户端的停止直播不进行回应
	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgStreamStopRsp(const char* json)//设备的停止推流回应
{
	if(!fAuthenticated)//没有进行认证请求
		return httpUnAuthorized;

	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgStreamStartReqRestful(char *queryString)//放到ProcessRequest所在的状态去处理，方便多次循环调用
{
	/*//暂时注释掉，实际上是需要认证的
	if(!fAuthenticated)//没有进行认证请求
	return httpUnAuthorized;
	*/
	QueryParamList parList(queryString);
	const char* chSerial	=	parList.DoFindCGIValueForParam("device");//获取设备序列号
	const char* chChannel	=	parList.DoFindCGIValueForParam("channel");//获取通道
	const char* chProtocol  =   parList.DoFindCGIValueForParam("protocol");//获取通道
	const char* chReserve	=	parList.DoFindCGIValueForParam("reserve");//获取通道

	//为可选参数填充默认值
	if(chChannel==NULL)
		chChannel="01";
	if(chReserve==NULL)
		chReserve="1";

	if(chSerial==NULL||chProtocol==NULL)
		return QTSS_BadArgument;

	EasyDarwin::Protocol::EasyDarwinRSP req(MSG_CS_GET_STREAM_REQ);//由restful接口合成json格式请求
	EasyJsonValue header,body;

	char chTemp[16]={0};//如果客户端不提供CSeq,那么我们每次给他生成一个唯一的CSeq
	UInt32 uCseq=GetCSeq();
	sprintf(chTemp,"%d",uCseq);

	header["CSeq"]		=		chTemp;
	header["Version"]	=		"1.0";
	body["Serial"]		=		chSerial;
	body["Channel"]		=		chChannel;
	body["Protocol"]	=		chProtocol;
	body["Reserve"]		=		chReserve;

	req.SetHead(header);
	req.SetBody(body);

	string buffer=req.GetMsg();
	fRequestBody =new char[buffer.size()+1];
	strcpy(fRequestBody,buffer.c_str());
	return QTSS_NoErr;

}
QTSS_Error HTTPSession::ExecNetMsgStreamStartReq(const char* json)//客户端开始流请求
{
	/*//暂时注释掉，实际上是需要认证的
	if(!fAuthenticated)//没有进行认证请求
	return httpUnAuthorized;
	*/

	EasyDarwin::Protocol::EasyProtocol req(json);
	string strCSeq=req.GetHeaderValue("CSeq");
	UInt32 uCSeq=atoi(strCSeq.c_str());
	string strURL;//直播地址

	string strDeviceSerial	=	req.GetBodyValue("Serial");//设备序列号
	string strCameraSerial	=	req.GetBodyValue("Channel");//摄像头序列号
	string strProtocol		=	req.GetBodyValue("Protocol");//协议
	string strStreamID		=	req.GetBodyValue("Reserve");//流类型

	//可选参数如果没有，则用默认值填充
	if(strCameraSerial.empty())//表示为EasyCamera设备
		strCameraSerial="01";
	if(strStreamID.empty())//没有码流需求时默认为标清
		strStreamID="1";

	if(strDeviceSerial.size()<=0||strProtocol.size()<=0)//参数判断
		return QTSS_BadArgument;

	if(fInfo.cWaitingState==0)//第一次处理请求
	{
		OSRefTableEx* DeviceMap=QTSServerInterface::GetServer()->GetDeviceMap();
		OSRefTableEx::OSRefEx* theDevRef=DeviceMap->Resolve(strDeviceSerial);////////////////////////////////++
		if(theDevRef==NULL)//找不到指定设备
			return EASY_ERROR_DEVICE_NOT_FOUND;

		//走到这说明存在指定设备
		HTTPSession * pDevSession=(HTTPSession *)theDevRef->GetObjectPtr();//获得当前设备回话
		string strDssIP,strDssPort;
		if(QTSServerInterface::GetServer()->RedisGetAssociatedDarWin(strDeviceSerial,strCameraSerial,strDssIP,strDssPort))//是否存在关联的EasyDarWin转发服务器test,应该用Redis上的数据，因为推流是不可靠的，而EasyDarWin上的数据是可靠的
		{
			//合成直播的RTSP地址，后续有可能根据请求流的协议不同而生成不同的直播地址，如RTMP、HLS等
			string strSessionID;
			bool bReval=QTSServerInterface::GetServer()->RedisGenSession(strSessionID,SessionIDTimeout);
			if(!bReval)//sessionID在redis上的存储失败
			{
				DeviceMap->Release(strDeviceSerial);/////////////////////////////////////////////--
				return EASY_ERROR_SERVER_INTERNAL_ERROR;
			}
			strURL="rtsp://"+strDssIP+':'+strDssPort+'/'
				+strSessionID+'/'
				+strDeviceSerial+'/'
				+strCameraSerial+".sdp";

			pDevSession->InsertToSet(strCameraSerial,this);//将当前客户端加入到拉流客户端列表中
			stStreamInfo stTemp;
			stTemp.strDeviceSerial=strDeviceSerial;
			stTemp.strCameraSerial=strCameraSerial;
			stTemp.strProtocol=strProtocol;
			stTemp.strStreamID=strStreamID;
			InsertToStreamMap(strDeviceSerial+strCameraSerial,stTemp);//以设备序列号+摄像头序列号作为唯一标识
			//下面已经用不到设备回话了，释放引用
			DeviceMap->Release(strDeviceSerial);/////////////////////////////////////////////--
		}
		else
		{//不存在关联的EasyDarWin
			bool bErr=QTSServerInterface::GetServer()->RedisGetBestDarWin(strDssIP,strDssPort);
			if(!bErr)//不存在DarWin
			{
				DeviceMap->Release(strDeviceSerial);/////////////////////////////////////////////--
				return EASY_ERROR_SERVICE_NOT_FOUND;
			}
			//向指定设备发送开始流请求

			EasyDarwin::Protocol::EasyDarwinRSP		reqreq(MSG_SD_PUSH_STREAM_REQ);
			EasyJsonValue headerheader,bodybody;

			char chTemp[16]={0};
			UInt32 uDevCseq=pDevSession->GetCSeq();
			sprintf(chTemp,"%d",uDevCseq);
			headerheader["CSeq"]	=string(chTemp);//注意这个地方不能直接将UINT32->int,因为会造成数据失真
			headerheader[EASY_TAG_VERSION]=		EASY_PROTOCOL_VERSION;


			string strSessionID;
			bool bReval=QTSServerInterface::GetServer()->RedisGenSession(strSessionID,SessionIDTimeout);
			if(!bReval)//sessionID再redis上的存储失败
			{
				DeviceMap->Release(strDeviceSerial);/////////////////////////////////////////////--
				return EASY_ERROR_SERVER_INTERNAL_ERROR;
			}

			bodybody["StreamID"]		=		strSessionID;
			bodybody["Server_IP"]		=		strDssIP;
			bodybody["Server_PORT"]		=		strDssPort;
			bodybody["Serial"]			=	strDeviceSerial;
			bodybody["Channel"]			=	strCameraSerial;
			bodybody["Protocol"]		=		strProtocol;
			bodybody["Reserve"]			=		strStreamID;

			reqreq.SetHead(headerheader);
			reqreq.SetBody(bodybody);

			string buffer = reqreq.GetMsg();
			//
			strMessage msgTemp;

			msgTemp.iMsgType=MSG_CS_GET_STREAM_REQ;//当前请求的消息
			msgTemp.pObject=this;//当前对象指针
			msgTemp.uCseq=uCSeq;//当前请求的cseq

			pDevSession->InsertToMsgMap(uDevCseq,msgTemp);//加入到Map中等待客户端的回应
			IncrementObjectHolderCount();//增加引用，防止设备回应时当前Session已经终止
			QTSS_SendHTTPPacket(pDevSession,(char*)buffer.c_str(),buffer.size(),false,false);
			DeviceMap->Release(strDeviceSerial);//////////////////////////////////////////////////////////--

			fInfo.cWaitingState=1;//等待设备回应
			fInfo.iResponse=0;//表示设备还没有回应
			fInfo.uTimeoutNum=0;//开始计算超时
			fInfo.uWaitingTime=100;//以100ms为周期循环等待，这样不占用CPU
			return QTSS_NoErr;
		}
	}
	else//等待设备回应 
	{
		if(fInfo.iResponse==0)//设备还没有回应
		{
			fInfo.uTimeoutNum++;
			if(fInfo.uTimeoutNum>CliStartStreamTimeout/100)//超时了
			{
				fInfo.cWaitingState=0;//恢复状态
				return httpRequestTimeout;
			}
			else//没有超时，继续等待
			{
				fInfo.uWaitingTime=100;//以100ms为周期循环等待，这样不占用CPU
				return QTSS_NoErr;
			}
		}
		else if(fInfo.uCseq!=uCSeq)//这个不是我想要的，可能是第一次请求时超时，第二次请求时返回了第一个的回应，这时我们应该继续等待第二个的回应直到超时
		{
			fInfo.iResponse=0;//继续等待，这一个可能和另一个线程同时赋值，加锁也不能解决，只不过影响不大。
			fInfo.uTimeoutNum++;
			fInfo.uWaitingTime=100;//以100ms为周期循环等待，这样不占用CPU
			return QTSS_NoErr;
		}
		else if(fInfo.iResponse==200)//正确回应
		{
			fInfo.cWaitingState=0;//恢复状态
			strStreamID=fInfo.strStreamID;//使用设备的流类型和推流协议
			strProtocol=fInfo.strProtocol;
			//合成直播地址

			string strSessionID;
			bool bReval=QTSServerInterface::GetServer()->RedisGenSession(strSessionID,SessionIDTimeout);
			if(!bReval)//sessionID在redis上的存储失败
			{
				return EASY_ERROR_SERVER_INTERNAL_ERROR;
			}

			strURL="rtsp://"+fInfo.strDssIP+':'+fInfo.strDssPort+'/'
				+strSessionID+'/'
				+strDeviceSerial+'/'
				+strCameraSerial+".sdp";

			//自动停止推流add
			OSRefTableEx* DeviceMap=QTSServerInterface::GetServer()->GetDeviceMap();
			OSRefTableEx::OSRefEx* theDevRef=DeviceMap->Resolve(strDeviceSerial);////////////////////////////////++
			if(theDevRef==NULL)//找不到指定设备
				return EASY_ERROR_DEVICE_NOT_FOUND;
			//走到这说明存在指定设备
			HTTPSession * pDevSession=(HTTPSession *)theDevRef->GetObjectPtr();//获得当前设备回话
			pDevSession->InsertToSet(strCameraSerial,this);//将当前客户端加入到拉流客户端列表中
			stStreamInfo stTemp;
			stTemp.strDeviceSerial=strDeviceSerial;
			stTemp.strCameraSerial=strCameraSerial;
			stTemp.strProtocol=strProtocol;
			stTemp.strStreamID=strStreamID;
			InsertToStreamMap(strDeviceSerial+strCameraSerial,stTemp);
			DeviceMap->Release(strDeviceSerial);////////////////////////////////////////////////--
			//自动停止推流add
		}
		else//设备错误回应
		{
			fInfo.cWaitingState=0;//恢复状态
			return fInfo.iResponse;
		}
	}

	//走到这说明对客户端的正确回应,因为错误回应直接返回。
	EasyDarwin::Protocol::EasyDarwinRSP rsp(MSG_SC_GET_STREAM_ACK);
	EasyJsonValue header,body;
	body["URL"]=strURL;
	body["Channel"]=strDeviceSerial;
	body["CameraSerial"]=strCameraSerial;
	body["Protocol"]=strProtocol;//如果当前已经推流，则返回请求的，否则返回实际推流类型
	body["Reserve"]=strStreamID;//如果当前已经推流，则返回请求的，否则返回实际推流类型

	header[EASY_TAG_VERSION]=EASY_PROTOCOL_VERSION;
	header["CSeq"]=strCSeq;
	header["ErrorNum"]=200;
	header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(200);


	rsp.SetHead(header);
	rsp.SetBody(body);
	string msg = rsp.GetMsg();

	//构造响应报文(HTTP Header)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);

	//将相应报文添加到HTTP输出缓冲区中
	if (!msg.empty()) 
		pOutputStream->Put((char*)msg.data(), msg.length());
	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgStartDeviceStreamRspEx(const char* json)//设备的开始流回应
{
	if(!fAuthenticated)//没有进行认证请求
		return httpUnAuthorized;

	//对于设备的推流回应是不需要在进行回应的，直接解析找到对应的客户端Session，赋值即可	
	EasyDarwin::Protocol::EasyProtocol req(json);


	string strDeviceSerial	=	req.GetBodyValue("Serial");//设备序列号
	string strCameraSerial	=	req.GetBodyValue("Channel");//摄像头序列号
	//string strProtocol		=	req.GetBodyValue("Protocol");//协议,终端仅支持RTSP推送
	string strStreamID		=	req.GetBodyValue("Reserve");//流类型
	string strDssIP         =   req.GetBodyValue("Server_IP");//设备实际推流地址
	string strDssPort       =   req.GetBodyValue("Server_Port");//和端口

	string strCSeq			=	req.GetHeaderValue("CSeq");//这个是关键字
	string strStateCode     =   req.GetHeaderValue("ErrorNum");//状态码

	UInt32 uCSeq=atoi(strCSeq.c_str());
	int iStateCode=atoi(strStateCode.c_str());

	strMessage strTempMsg;
	if(!FindInMsgMap(uCSeq,strTempMsg))
	{//天啊，竟然找不到，一定是设备发送的CSeq和它接收的不一样
		return QTSS_BadArgument;
	}
	else
	{
		HTTPSession * pCliSession=(HTTPSession *)strTempMsg.pObject;//这个对象指针是有效的，因为之前我们给他加了回命草
		if(strTempMsg.iMsgType==MSG_CS_GET_STREAM_REQ)//客户端的开始流请求
		{
			if(iStateCode==200)//只有正确回应才进行一些信息的保存
			{
				pCliSession->fInfo.strDssIP=strDssIP;
				pCliSession->fInfo.strDssPort=strDssPort;
				pCliSession->fInfo.strStreamID=strStreamID;
				//pCliSession->fInfo.strProtocol=strProtocol;
			}
			pCliSession->fInfo.uCseq=strTempMsg.uCseq;
			pCliSession->fInfo.iResponse=iStateCode;//这句之后开始触发客户端对象
			pCliSession->DecrementObjectHolderCount();//现在可以放心的安息了
		}
		else
		{

		}
	}
	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgGetDeviceListReqEx(char *queryString)//客户端获得设备列表
{
	//queryString在这个函数中是没有用的，仅为了保持接口的一致性。
	/*
	if(!fAuthenticated)//没有进行认证请求
	return httpUnAuthorized;
	*/

	EasyDarwin::Protocol::EasyDarwinRSP		rsp(MSG_SC_DEVICE_LIST_ACK);
	EasyJsonValue header,body;

	header[EASY_TAG_VERSION]=EASY_PROTOCOL_VERSION;
	header["CSeq"]=1;
	header["ErrorNum"]=200;
	header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(200);


	OSMutex *mutexMap=QTSServerInterface::GetServer()->GetDeviceMap()->GetMutex();
	OSHashMap  *deviceMap=QTSServerInterface::GetServer()->GetDeviceMap()->GetMap();
	OSRefIt itRef;
	Json::Value *proot=rsp.GetRoot();

	mutexMap->Lock();
	body["DeviceCount"]=QTSServerInterface::GetServer()->GetDeviceMap()->GetEleNumInMap();
	for(itRef=deviceMap->begin();itRef!=deviceMap->end();itRef++)
	{
		Json::Value value;
		strDevice *deviceInfo=((HTTPSession*)(itRef->second->GetObjectPtr()))->GetDeviceInfo();
		value["Serial"]		=	deviceInfo->serial_;
		value["Name"]		=	deviceInfo->name_;
		value["Tag"]		=	deviceInfo->tag_;
		value["AppType"]	=	EasyProtocol::GetAppTypeString(deviceInfo->eAppType);
		value["TerminalType"]	=	EasyProtocol::GetTerminalTypeString(deviceInfo->eDeviceType);
		(*proot)[EASY_TAG_ROOT][EASY_TAG_BODY]["Devices"].append(value);
	}
	mutexMap->Unlock();

	rsp.SetHead(header);
	rsp.SetBody(body);
	string msg = rsp.GetMsg();

	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//响应完成后断开连接
	httpAck.AppendConnectionCloseHeader();

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (!msg.empty())
		pOutputStream->Put((char*)msg.data(), msg.length());

	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgGetDeviceListReqJsonEx(const char *json)//客户端获得设备列表
{
	/*
	if(!fAuthenticated)//没有进行认证请求
	return httpUnAuthorized;
	*/
	EasyDarwin::Protocol::EasyProtocol		req(json);


	EasyDarwin::Protocol::EasyDarwinRSP		rsp(MSG_SC_DEVICE_LIST_ACK);
	EasyJsonValue header,body;

	header[EASY_TAG_VERSION]=EASY_PROTOCOL_VERSION;
	header["CSeq"]=req.GetHeaderValue("CSeq");
	header["ErrorNum"]=200;
	header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(200);


	OSMutex *mutexMap=QTSServerInterface::GetServer()->GetDeviceMap()->GetMutex();
	OSHashMap  *deviceMap=QTSServerInterface::GetServer()->GetDeviceMap()->GetMap();
	OSRefIt itRef;
	Json::Value *proot=rsp.GetRoot();

	mutexMap->Lock();
	body["DeviceCount"]=QTSServerInterface::GetServer()->GetDeviceMap()->GetEleNumInMap();
	for(itRef=deviceMap->begin();itRef!=deviceMap->end();itRef++)
	{
		Json::Value value;
		strDevice *deviceInfo=((HTTPSession*)(itRef->second->GetObjectPtr()))->GetDeviceInfo();
		value["Serial"]	=	deviceInfo->serial_;
		value["Name"]		=	deviceInfo->name_;
		value["Tag"]		=	deviceInfo->tag_;
		value["AppType"]	=	EasyProtocol::GetAppTypeString(deviceInfo->eAppType);
		value["TerminalType"]	=	EasyProtocol::GetTerminalTypeString(deviceInfo->eDeviceType);
		(*proot)[EASY_TAG_ROOT][EASY_TAG_BODY]["Devices"].append(value);
	}
	mutexMap->Unlock();

	rsp.SetHead(header);
	rsp.SetBody(body);
	string msg = rsp.GetMsg();

	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (!msg.empty())
		pOutputStream->Put((char*)msg.data(), msg.length());

	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgGetCameraListReqEx(char* queryString)
{
	/*	
	if(!fAuthenticated)//没有进行认证请求
	return httpUnAuthorized;
	*/

	QueryParamList parList(queryString);
	const char* device_serial = parList.DoFindCGIValueForParam("device");//获取设备序列号

	if(device_serial==NULL)
		return QTSS_BadArgument;

	EasyDarwin::Protocol::EasyDarwinRSP		rsp(MSG_SC_CAMERA_LIST_ACK);
	EasyJsonValue header,body;

	header[EASY_TAG_VERSION]=EASY_PROTOCOL_VERSION;
	header["CSeq"]=1;

	body["Serial"]=device_serial;

	OSRefTableEx* DeviceMap=QTSServerInterface::GetServer()->GetDeviceMap();
	OSRefTableEx::OSRefEx* theDevRef=DeviceMap->Resolve(device_serial);////////////////////////////////++
	if(theDevRef==NULL)//不存在指定设备
	{
		header["ErrorNum"]=603;
		header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(EASY_ERROR_DEVICE_NOT_FOUND);
	}
	else//存在指定设备，则获取这个设备的摄像头信息
	{
		header["ErrorNum"]=200;
		header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(200);

		Json::Value *proot=rsp.GetRoot();
		strDevice *deviceInfo= ((HTTPSession*)theDevRef->GetObjectPtr())->GetDeviceInfo();
		if(deviceInfo->eAppType==EASY_APP_TYPE_CAMERA)
		{
			body["SnapURL"]=deviceInfo->snapJpgPath_;
		}
		else
		{
			EasyDevices *camerasInfo=&(deviceInfo->cameras_);
			EasyDevicesIterator itCam;
			body["ChannelCount"]=((HTTPSession*)theDevRef->GetObjectPtr())->GetDeviceInfo()->channelCount_;
			for(itCam=camerasInfo->begin();itCam!=camerasInfo->end();itCam++)
			{
				Json::Value value;
				value["Channel"]=itCam->channel_;
				value["Name"]=itCam->name_;
				value["Status"]=itCam->status_;
				value["SnapURL"]=itCam->snapJpgPath_;
				(*proot)[EASY_TAG_ROOT][EASY_TAG_BODY]["Cameras"].append(value);
			}
		}
		DeviceMap->Release(device_serial);////////////////////////////////--
	}
	rsp.SetHead(header);
	rsp.SetBody(body);
	string msg = rsp.GetMsg();

	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//响应完成后断开连接
	httpAck.AppendConnectionCloseHeader();

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (!msg.empty())
		pOutputStream->Put((char*)msg.data(), msg.length());

	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ExecNetMsgGetCameraListReqJsonEx(const char* json)
{
	/*	
	if(!fAuthenticated)//没有进行认证请求
	return httpUnAuthorized;
	*/

	EasyDarwin::Protocol::EasyProtocol      req(json);
	string strDeviceSerial	=	req.GetBodyValue("Serial");

	if(strDeviceSerial.size()<=0)
		return QTSS_BadArgument;

	EasyDarwin::Protocol::EasyDarwinRSP		rsp(MSG_SC_CAMERA_LIST_ACK);
	EasyJsonValue header,body;

	header[EASY_TAG_VERSION]=EASY_PROTOCOL_VERSION;
	header["CSeq"]=req.GetHeaderValue("CSeq");
	header["ErrorNum"]=200;
	header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(200);
	body["Serial"]=strDeviceSerial;

	OSRefTableEx* DeviceMap=QTSServerInterface::GetServer()->GetDeviceMap();
	OSRefTableEx::OSRefEx* theDevRef=DeviceMap->Resolve(strDeviceSerial);////////////////////////////////++
	if(theDevRef==NULL)//不存在指定设备
	{
		return EASY_ERROR_DEVICE_NOT_FOUND;//交给错误处理中心处理
	}
	else//存在指定设备，则获取这个设备的摄像头信息
	{

		Json::Value *proot=rsp.GetRoot();
		strDevice *deviceInfo= ((HTTPSession*)theDevRef->GetObjectPtr())->GetDeviceInfo();
		if(deviceInfo->eAppType==EASY_APP_TYPE_CAMERA)
		{
			body["SnapURL"]=deviceInfo->snapJpgPath_;
		}
		else
		{
			EasyDevices *camerasInfo=&(deviceInfo->cameras_);
			EasyDevicesIterator itCam;

			body["ChannelCount"]=((HTTPSession*)theDevRef->GetObjectPtr())->GetDeviceInfo()->channelCount_;
			for(itCam=camerasInfo->begin();itCam!=camerasInfo->end();itCam++)
			{
				Json::Value value;
				value["Channel"]=itCam->channel_;
				value["Name"]=itCam->name_;
				value["Status"]=itCam->status_;
				body["SnapURL"]=itCam->snapJpgPath_;
				(*proot)[EASY_TAG_ROOT][EASY_TAG_BODY]["Cameras"].append(value);
			}
		}
		DeviceMap->Release(strDeviceSerial);////////////////////////////////--
	}
	rsp.SetHead(header);
	rsp.SetBody(body);
	string msg = rsp.GetMsg();

	//构造响应报文(HTTP头)
	HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
	httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
	if (!msg.empty())
		httpAck.AppendContentLengthHeader((UInt32)msg.length());

	//响应完成后断开连接
	httpAck.AppendConnectionCloseHeader();

	//Push MSG to OutputBuffer
	char respHeader[2048] = { 0 };
	StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
	strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

	HTTPResponseStream *pOutputStream = GetOutputStream();
	pOutputStream->Put(respHeader);
	if (!msg.empty())
		pOutputStream->Put((char*)msg.data(), msg.length());

	return QTSS_NoErr;
}
QTSS_Error HTTPSession::ProcessRequest()//处理请求
{
	//OSCharArrayDeleter charArrayPathDeleter(theRequestBody);//不要在这删除，因为可能执行多次，仅当对请求的处理完毕后再进行删除

	if(fRequestBody==NULL)//表示没有正确的解析请求，SetUpRequest环节没有解析出数据部分
		return QTSS_NoErr;


	//消息处理
	QTSS_Error theErr=QTSS_NoErr;

	EasyDarwin::Protocol::EasyProtocol protocol(fRequestBody);
	int nNetMsg = protocol.GetMessageType(),nRspMsg=MSG_SC_EXCEPTION;

	switch (nNetMsg)
	{
	case MSG_DS_REGISTER_REQ://处理设备上线消息,设备类型包括NVR、摄像头和智能主机
		theErr=ExecNetMsgDevRegisterReqEx(fRequestBody);
		nRspMsg=MSG_SD_REGISTER_ACK;
		break;
	case MSG_CS_GET_STREAM_REQ://客户端的开始流请求
		theErr=ExecNetMsgStreamStartReq(fRequestBody);
		nRspMsg=MSG_SC_GET_STREAM_ACK;
		break;
	case MSG_DS_PUSH_STREAM_ACK://设备的开始流回应
		theErr=ExecNetMsgStartDeviceStreamRspEx(fRequestBody);
		nRspMsg=MSG_DS_PUSH_STREAM_ACK;//注意，这里实际上是不应该再回应的
		break;
	case MSG_CS_FREE_STREAM_REQ://客户端的停止直播请求
		theErr=ExecNetMsgStreamStopReq(fRequestBody);
		nRspMsg=MSG_SC_FREE_STREAM_ACK;
		break;
	case MSG_DS_STREAM_STOP_ACK://设备对CMS的停止推流回应
		theErr=ExecNetMsgStreamStopRsp(fRequestBody);
		nRspMsg=MSG_DS_STREAM_STOP_ACK;//注意，这里实际上是不需要在进行回应的
		break;
	case MSG_CS_DEVICE_LIST_REQ://设备列表请求
		theErr=ExecNetMsgGetDeviceListReqJsonEx(fRequestBody);//add
		nRspMsg=MSG_SC_DEVICE_LIST_ACK;
		break;
	case MSG_CS_CAMERA_LIST_REQ://摄像头列表请求
		theErr=ExecNetMsgGetCameraListReqJsonEx(fRequestBody);//add
		nRspMsg=MSG_SC_CAMERA_LIST_ACK;
		break;
	case MSG_DS_POST_SNAP_REQ:
		theErr=ExecNetMsgSnapUpdateReq(fRequestBody);
		nRspMsg=MSG_SD_POST_SNAP_ACK;
	default:
		theErr=ExecNetMsgDefaultReqHandler(fRequestBody);
		break;
	}
	//如果不想进入错误自动处理则一定要返回QTSS_NoErr
	if(theErr!=QTSS_NoErr)//无论是正确回应还是等待返回都是QTSS_NoErr，出现错误，对错误进行统一回应
	{
		EasyDarwin::Protocol::EasyProtocol req(fRequestBody);
		EasyDarwin::Protocol::EasyDarwinRSP rsp(nRspMsg);
		EasyJsonValue header;
		header[EASY_TAG_VERSION]=EASY_PROTOCOL_VERSION;
		header["CSeq"]=req.GetHeaderValue("CSeq");

		switch(theErr)
		{
		case QTSS_BadArgument:
			header["ErrorNum"]=400;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(400);
			break;
		case httpUnAuthorized:
			header["ErrorNum"]=401;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(401);
			break;
		case QTSS_AttrNameExists:
			header["ErrorNum"]=409;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(409);
			break;
		case EASY_ERROR_DEVICE_NOT_FOUND:
			header["ErrorNum"]=603;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(603);
			break;
		case EASY_ERROR_SERVICE_NOT_FOUND:
			header["ErrorNum"]=605;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(605);
			break;
		case httpRequestTimeout:
			header["ErrorNum"]=408;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(408);
			break;
		case EASY_ERROR_SERVER_INTERNAL_ERROR:
			header["ErrorNum"]=500;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(500);
			break;
		case EASY_ERROR_SERVER_NOT_IMPLEMENTED:
			header["ErrorNum"]=501;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(501);
			break;
		default:
			header["ErrorNum"]=400;
			header["ErrorString"]=EasyDarwin::Protocol::EasyProtocol::GetErrorString(400);
			break;
		}

		rsp.SetHead(header);
		string msg = rsp.GetMsg();
		//构造响应报文(HTTP Header)
		HTTPRequest httpAck(&QTSServerInterface::GetServerHeader(), httpResponseType);
		httpAck.CreateResponseHeader(!msg.empty()?httpOK:httpNotImplemented);
		if (!msg.empty())
			httpAck.AppendContentLengthHeader((UInt32)msg.length());

		char respHeader[2048] = { 0 };
		StrPtrLen* ackPtr = httpAck.GetCompleteHTTPHeader();
		strncpy(respHeader,ackPtr->Ptr, ackPtr->Len);

		HTTPResponseStream *pOutputStream = GetOutputStream();
		pOutputStream->Put(respHeader);

		//将相应报文添加到HTTP输出缓冲区中
		if (!msg.empty()) 
			pOutputStream->Put((char*)msg.data(), msg.length());
	}
	return QTSS_NoErr;
}
//公共，end

