/*
	Copyleft (c) 2013-2015 EasyDarwin.ORG.  All rights reserved.
	Github: https://github.com/EasyDarwin
	WEChat: EasyDarwin
	Website: http://www.EasyDarwin.org
*/
/*! 
  \file    ServiceSession.h  
  \author  Babosa@EasyDarwin.org
  \date    2014-12-03
  \version 1.0
  \mainpage 使用引导
  
  网络调用主要流程\n
  Select -> ServiceSession -> DispatchMsgCenter -> ServiceSession -> Cleanup\n\n

  Copyright (c) 2014 EasyDarwin.org 版权所有\n  
 
  \defgroup 服务单元网络事件处理流程
*/

#ifndef __HTTP_SESSION_H__
#define __HTTP_SESSION_H__

#include "HTTPSessionInterface.h"
#include "HTTPRequest.h"
#include "TimeoutTask.h"
#include "QTSSModule.h"

using namespace std;


class HTTPSession : public HTTPSessionInterface
{
    public:
        HTTPSession();
        virtual ~HTTPSession();
		
		////发送HTTP响应报文
		virtual QTSS_Error SendHTTPPacket(StrPtrLen* contentXML, Bool16 connectionClose, Bool16 decrement);

		char* GetDeviceSnap(){ return fDeviceSnap; };
		char* GetDeviceSerial(){ return (char*)fDevSerial.c_str(); };
		
		void SetStreamPushInfo(EasyJsonValue &info) { fStreamPushInfo = info; }
		EasyJsonValue &GetStreamPushInfo() { return fStreamPushInfo; }

		

    private: 
        SInt64 Run();

        // Does request prep & request cleanup, respectively
        QTSS_Error SetupRequest();
        void CleanupRequest();
		
		//保留，begin
		QTSS_Error ExecNetMsgDevRegisterReq(const char* json);
		QTSS_Error ExecNetMsgGetDeviceListReq(char *queryString);
		QTSS_Error ExecNetMsgGetCameraListReq(const string& device_serial, char* queryString);
		QTSS_Error ExecNetMsgStartStreamReq(const string& device_serial, char* queryString);
		QTSS_Error ExecNetMsgStopStreamReq(const string& device_serial, char* queryString);
		QTSS_Error ExecNetMsgStartDeviceStreamRsp(const char* json);
		QTSS_Error ExecNetMsgStopDeviceStreamRsp(const char* json);
		//保留，end

		
		QTSS_Error ProcessRequest();//处理请求，单独放到一个状态中去处理，这样方便重复执行
		QTSS_Error ExecNetMsgDefaultReqHandler(const char* json);//消息默认处理函数
		QTSS_Error ExecNetMsgDevRegisterReqEx(const char* json);//设备注册请求
		QTSS_Error ExecNetMsgStreamStartReqRestful(char *queryString);//客户端拉流请求，Restful接口
		QTSS_Error ExecNetMsgStreamStartReq(const char* json);//客户端拉流请求
		QTSS_Error ExecNetMsgStartDeviceStreamRspEx(const char* json);//设备的开始流回应
		QTSS_Error ExecNetMsgStreamStopReqRestful(char *queryString);//客户端的停止直播请求，Restful接口
		QTSS_Error ExecNetMsgStreamStopReq(const char *json);//客户端的停止直播请求
		QTSS_Error ExecNetMsgStreamStopRsp(const char* json);//设备的停止推流回应
		QTSS_Error ExecNetMsgSnapUpdateReq(const char* json);//设备的快照更新请求
		QTSS_Error ExecNetMsgGetDeviceListReqEx(char *queryString);//客户端获得设备列表,restful接口
		QTSS_Error ExecNetMsgGetDeviceListReqJsonEx(const char *json);//客户端获得设备列表，json接口
		QTSS_Error ExecNetMsgGetCameraListReqEx(char *queryString);//客户端获得摄像头列表，restful接口，仅对设备类型为NVR时有效
		QTSS_Error ExecNetMsgGetCameraListReqJsonEx(const char *json);//客户端获得摄像头列表，json接口,仅对设备类型为NVR时有效


        // test current connections handled by this object against server pref connection limit
        Bool16 OverMaxConnections(UInt32 buffer);

        HTTPRequest*        fRequest;
        OSMutex             fReadMutex;

		//网络报文处理状态机
        enum
        {
            kReadingRequest             = 0,	//读取报文
            kFilteringRequest           = 1,	//过滤报文
            kPreprocessingRequest       = 2,	//预处理报文
            kProcessingRequest          = 3,	//处理报文
            kSendingResponse            = 4,	//发送响应报文
            kCleaningUp                 = 5,	//清空本次处理的报文内容
        
            kReadingFirstRequest		= 6,	//第一次读取Session报文，主要用来做Session协议区分（HTTP/TCP/RTSP等等）
            kHaveCompleteMessage		= 7    // 读取到完整的报文
        };
        
        UInt32 fCurrentModule;
        UInt32 fState;

        QTSS_RoleParams     fRoleParams;//module param blocks for roles.
        QTSS_ModuleState    fModuleState;

		//清空请求报文
        QTSS_Error DumpRequestData();

		char* fDeviceSnap;
		EasyJsonValue fStreamPushInfo;
};
#endif // __HTTP_SESSION_H__

