/*
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * Copyright (c) 1999-2008 Apple Inc.  All Rights Reserved.
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 *
 */
/*
	Copyleft (c) 2013-2015 EasyDarwin.ORG.  All rights reserved.
	Github: https://github.com/EasyDarwin
	WEChat: EasyDarwin
	Website: http://www.EasyDarwin.org
*/
/*
    File:       HTTPSessionInterface.h

    Contains:   Presents an API for session-wide resources for modules to use.
                Implements the CMS Session dictionary for QTSS API. 
*/

#ifndef __HTTPSESSIONINTERFACE_H__
#define __HTTPSESSIONINTERFACE_H__

#include "HTTPRequestStream.h"
#include "HTTPResponseStream.h"
#include "Task.h"
#include "QTSS.h"
#include "QTSSDictionary.h"
#include "atomic.h"
#include "base64.h"
#include <string>
#include <boost/thread/condition.hpp>

#include "OSRefTableEx.h"
#include "EasyProtocolDef.h"
#include "EasyProtocol.h"
#include <map>

using namespace EasyDarwin::Protocol;
using namespace std;

class HTTPSessionInterface : public QTSSDictionary, public Task
{
public:

    //Initialize must be called right off the bat to initialize dictionary resources
    static void     Initialize();
    
    HTTPSessionInterface();
    virtual ~HTTPSessionInterface();
    
    //Is this session alive? If this returns false, clean up and begone as
    //fast as possible
    Bool16 IsLiveSession()      { return fSocket.IsConnected() && fLiveSession; }
    
    // Allows clients to refresh the timeout
    void RefreshTimeout()       { fTimeoutTask.RefreshTimeout(); }

    // In order to facilitate sending out of band data on the RTSP connection,
    // other objects need to have direct pointer access to this object. But,
    // because this object is a task object it can go away at any time. If # of
    // object holders is > 0, the RTSPSession will NEVER go away. However,
    // the object managing the session should be aware that if IsLiveSession returns
    // false it may be wise to relinquish control of the session
    void IncrementObjectHolderCount() { (void)atomic_add(&fObjectHolders, 1); }
    void DecrementObjectHolderCount();
    
    //Two main things are persistent through the course of a session, not
    //associated with any one request. The RequestStream (which can be used for
    //getting data from the client), and the socket. OOps, and the ResponseStream
    HTTPRequestStream*  GetInputStream()    { return &fInputStream; }
    HTTPResponseStream* GetOutputStream()   { return &fOutputStream; }
    TCPSocket*          GetSocket()         { return &fSocket; }
    OSMutex*            GetSessionMutex()   { return &fSessionMutex; }
    
    UInt32              GetSessionIndex()      { return fSessionIndex; }
    
    // Request Body Length
    // This object can enforce a length of the request body to prevent callers
    // of Read() from overrunning the request body and going into the next request.
    // -1 is an unknown request body length. If the body length is unknown,
    // this object will do no length enforcement. 
    void                SetRequestBodyLength(SInt32 inLength)   { fRequestBodyLen = inLength; }
    SInt32              GetRemainingReqBodyLen()                { return fRequestBodyLen; }

	//OSRef*	GetRef()	{return &fDevRef; }
	QTSS_Error RegDevSession(const char* serial, UInt32 serailLen);
	QTSS_Error UpdateDevRedis();
	QTSS_Error UpdateDevSnap(const char* inSnapTime, const char* inSnapJpg);
	void UnRegDevSession();
    
    // QTSS STREAM FUNCTIONS
    
    // Allows non-buffered writes to the client. These will flow control.
    
    //THE FIRST ENTRY OF THE IOVEC MUST BE BLANK!!!
    virtual QTSS_Error WriteV(iovec* inVec, UInt32 inNumVectors, UInt32 inTotalLength, UInt32* outLenWritten);
    virtual QTSS_Error Write(void* inBuffer, UInt32 inLength, UInt32* outLenWritten, UInt32 inFlags);
    virtual QTSS_Error Read(void* ioBuffer, UInt32 inLength, UInt32* outLenRead);
    virtual QTSS_Error RequestEvent(QTSS_EventType inEventMask);

	virtual QTSS_Error SendHTTPPacket(StrPtrLen* contentXML, Bool16 connectionClose, Bool16 decrement);

    enum
    {
        kMaxUserNameLen         = 32,
        kMaxUserPasswordLen     = 32
    };

	UInt32 GetStreamReqCount(string camera);
	void IncrementStreamReqCount(string camera);
	void DecrementStreamReqCount(string camera);

	void PushNVRMessage(EasyNVRMessage &msg) { fNVRMessageQueue.push_back(msg); }

protected:
    enum
    {
        kFirstCMSSessionID     = 1,    //UInt32
    };

    //Each RTSP session has a unique number that identifies it.

    char                fUserNameBuf[kMaxUserNameLen];
    char                fUserPasswordBuf[kMaxUserPasswordLen];
    char                fSessionID[QTSS_MAX_SESSION_ID_LENGTH];
	char				fLastSMSSessionID[QTSS_MAX_SESSION_ID_LENGTH];

	//char				fSerial[EASY_MAX_SERIAL_LENGTH];
	//StrPtrLen			fDevSerialPtr;
	string				fDevSerial;
	map<string, int>	fStreamReqCount;	//记录客户端请求打开视频次数
	boost::mutex		fNVROperatorMutex;
	boost::mutex		fStreamReqCountMutex;
	boost::condition_variable fCond;
	EasyNVRMessageQueue fNVRMessageQueue;
	//OSRef				fDevRef;

    TimeoutTask         fTimeoutTask;//allows the session to be timed out
    
    HTTPRequestStream   fInputStream;
    HTTPResponseStream  fOutputStream;
    
    // Any RTP session sending interleaved data on this RTSP session must
    // be prevented from writing while an RTSP request is in progress
    OSMutex             fSessionMutex;


    //+rt  socket we get from "accept()"
    TCPSocket           fSocket;
    TCPSocket*          fOutputSocketP;
    TCPSocket*          fInputSocketP;  // <-- usually same as fSocketP, unless we're HTTP Proxying
    
    void        SnarfInputSocket( HTTPSessionInterface* fromRTSPSession );
    
    // What session type are we?
    QTSS_SessionType    fSessionType;//普通socket,NVR,智能主机，摄像机
    Bool16              fLiveSession;
    unsigned int        fObjectHolders;

    UInt32              fSessionIndex;
    UInt32              fLocalAddr;
    UInt32              fRemoteAddr;
    SInt32              fRequestBodyLen;
    
    UInt16              fLocalPort;
    UInt16              fRemotePort;
    
	Bool16				fAuthenticated;

    static unsigned int		sSessionIndexCounter;

    // Dictionary support Param retrieval function
    static void*        SetupParams(QTSSDictionary* inSession, UInt32* outLen);
    
    static QTSSAttrInfoDict::AttrInfo   sAttributes[];
	
	//add,紫光，start
	#define CliStartStreamTimeout 30000//客户端开始流超时时间，单位为ms
	#define CliSNapShotTimeout    30000//客户端抓拍超时时间，单位为ms  
	#define SessionIDTimeout		  30000//sessionID在redis上的存活时间，单位为ms

	strDevice fDevice;//add,存储设备信息，仅当Session类型是设备时有效
	struct strInfo
	{
		int iResponse;//设备的回应码
		UInt32 uTimeoutNum;//循环等待次数
		char cWaitingState;//等待状态标志
		UInt32 uWaitingTime;//等待时间>0时需要等待


		UInt32 uCseq;//客户端的CSeq
		string strDssIP;//设备实际推流的地址
		string strDssPort;
		string strStreamID;//设备实际推流的类型
		string strProtocol;//设备实际推流协议
		
		string strType;//抓拍图片类型
		string strSnapShot;//设备抓拍图片的base64编码
	};
	struct strMessage
	{
		UInt32 uCseq;//客户端消息的CSeq
		void *pObject;//Session对象指针
		int iMsgType;//消息类型
	};
	typedef map<UInt32,strMessage> MsgMap;
	char *fRequestBody;//存储请求的数据部分

	OSMutex fMutexCSeq;//fCSeq互斥操作实现，因为可能多个线程同时fCSeq++,和MsgMap共同使用一个互斥量
	UInt32 fCSeq;//当前Session向对方发送请求时，fCSeq每次加1
	MsgMap fMsgMap;//存储客户端发来的消息
	strInfo fInfo;//存储设备发来的消息
	
	//自动停止推流实现
	DevMap fDevmap;//当前设备存储的拉流客户端列表,每一个摄像头都有一个拉流客户端列表
	OSMutex fMutexSet;//set、map互斥操作
	CliStreamMap fClientStreamMap;//当前客户端正在拉流的所有流信息
	//自动停止推流实现
public:
	strDevice *GetDeviceInfo(){return &fDevice;}
	UInt32 GetCSeq(){OSMutexLocker MutexLocker(&fMutexCSeq);return fCSeq++;}
	void  InsertToMsgMap(UInt32 uCSeq,strMessage& strMsg){OSMutexLocker MutexLocker(&fMutexCSeq);fMsgMap[uCSeq]=strMsg;}
	bool  FindInMsgMap(UInt32 uCSeq,strMessage& strMsg)
	{
		OSMutexLocker MutexLocker(&fMutexCSeq);
		MsgMap::iterator it_l=fMsgMap.find(uCSeq);
		if(it_l==fMsgMap.end())//没有找到
			return false;
		else
		{
			strMsg=it_l->second;
			fMsgMap.erase(it_l);
			return true;
		}
	}
	void ReleaseMsgMap()//释放对于对象的引用,否则对象将一直存在，无法得到释放
	{
		OSMutexLocker MutexLocker(&fMutexCSeq);
		MsgMap::iterator it_l;
		for(it_l=fMsgMap.begin();it_l!=fMsgMap.end();it_l++)
		{
			//if(it_l->second.iMsgType==MSG_CLI_CMS_STREAM_START_REQ||it_l->second.iMsgType==MSG_CLI_CMS_START_RECORD_REQ||it_l->second.iMsgType==MSG_CLI_CMS_SNAP_SHOT_REQ)
			((HTTPSessionInterface*)(it_l->second.pObject))->DecrementObjectHolderCount();//减少引用
		}
	}
	//自动停止推流操作
	void InsertToSet(const string &strCameraSerial,void * pObject);//加入到set中
	bool EraseInSet(const string &strCameraSerial,void *pObject);//删除元素，并判断是否为空，为空返回true,失败返回false
	void AutoStopStreamJudge();//自动停止推流判断，客户端连接断开时去判断,保证客户端直播后没有进行主动停止直播而直接断开连接这种情况不会让推流一直进行。
	void InsertToStreamMap(const string & strDeviceSerial,stStreamInfo & stTemp){OSMutexLocker MutexLocker(&fMutexSet);fClientStreamMap[strDeviceSerial]=stTemp;}
	bool FindInStreamMap(const string & strDeviceSerial,stStreamInfo & stTemp)
	{
		OSMutexLocker MutexLocker(&fMutexSet);
		CliStreamMap::iterator it_l=fClientStreamMap.find(strDeviceSerial);
		if(it_l==fClientStreamMap.end())//没有找到
			return false;
		else
		{
			stTemp=it_l->second;
			fClientStreamMap.erase(it_l);
			return true;
		}
	}
	//自动停止推流操作
	//add,紫光，end
};
#endif // __HTTPSESSIONINTERFACE_H__

