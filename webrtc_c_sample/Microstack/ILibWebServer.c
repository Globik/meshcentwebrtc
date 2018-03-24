/*
Copyright 2006 - 2015 Intel Corporation

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/

#define HTTPVERSION "1.1"

#if defined(WIN32) && !defined(_WIN32_WCE) && !defined(_MINCORE)
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif

#if defined(WIN32) && !defined(snprintf) && (_MSC_PLATFORM_TOOLSET <= 120)
#define fucksnprintf(dst, len, frm, ...) _snprintf_s(dst, len, _TRUNCATE, frm, __VA_ARGS__)
#endif

#if defined(WINSOCK2)
#include <winsock2.h>
#include <ws2tcpip.h>
#elif defined(WINSOCK1)
#include <winsock.h>
#include <wininet.h>
#endif

#include "ILibParsers.h"
#include "ILibWebServer.h"
#include "ILibAsyncServerSocket.h"
#include "ILibAsyncSocket.h"
#include "ILibWebClient.h"

#ifndef MICROSTACK_NOTLS
#include <openssl/sha.h>
#include "../core/utils.h"
#else
#include "sha1.h"
#endif
#include "ILibRemoteLogging.h"

#define WEBSOCKET_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WEBSOCKET_FIN		0x08000
#define WEBSOCKET_RSV1		0x04000
#define WEBSOCKET_RSV2		0x02000
#define WEBSOCKET_RSV3		0x01000
#define WEBSOCKET_OPCODE	0x00F00
#define WEBSOCKET_MASK		0x00080
#define WEBSOCKET_PLEN		0x0007F

#define WEBSOCKET_MAX_OUTPUT_FRAMESIZE 4096

#define WEBSOCKET_OPCODE_FRAMECONT		0x0
#define WEBSOCKET_OPCODE_TEXTFRAME		0x1
#define WEBSOCKET_OPCODE_BINARYFRAME	0x2
#define WEBSOCKET_OPCODE_CLOSE			0x8
#define WEBSOCKET_OPCODE_PING			0x9
#define WEBSOCKET_OPCODE_PONG			0xA

#define DIGEST_AUTHENTICATION_NONCE_DEFAULT_DURATION_MINUTES 15
//#define HTTP_SESSION_IDLE_TIMEOUT 30

#ifdef ILibWebServer_SESSION_TRACKING
void ILibWebServer_SessionTrack(void *Session, char *msg)
{
#if defined(WIN32) || defined(_WIN32_WCE)
	char tempMsg[4096];
	wchar_t t[4096];
	size_t len;
	sprintf_s(tempMsg, 4096, "Session: %p   %s\r\n", Session, msg);
	mbstowcs_s(&len, t, 4096, tempMsg, 4096);
	OutputDebugString(t);
#else
	printf("Session: %x   %s\r\n",Session,msg);
#endif
}
#define SESSION_TRACK(Session,msg) ILibWebServer_SessionTrack(Session,msg)
#else
#define SESSION_TRACK(Session,msg)
#endif
struct ILibWebServer_VirDir_Data
{
	voidfp callback;
	void *user;
};

typedef struct ILibWebServer_StateModule
{
	void(*PreSelect)(void* object, fd_set *readset, fd_set *writeset, fd_set *errorset, int* blocktime);
	void(*PostSelect)(void* object, int slct, fd_set *readset, fd_set *writeset, fd_set *errorset);
	void(*Destroy)(void* object);

	void *Chain;
	void *ServerSocket;
	void *LifeTime;
	void *User;
	void *Tag;

	void *VirtualDirectoryTable;

	int DigestAuth_NonceDuration;

	void(*OnSession)(struct ILibWebServer_Session *SessionToken, void *User);
}ILibWebServer_StateModule;

unsigned int ILibWebServer_Session_GetPendingBytesToSend(struct ILibWebServer_Session *session)
{
	return(ILibAsyncServerSocket_GetPendingBytesToSend(session->Reserved1, session->Reserved2));
}

/*! \fn ILibWebServer_SetTag(ILibWebServer_ServerToken object, void *Tag)
\brief Sets the user tag associated with the server
\param object The ILibWebServer to associate the user tag with
\param Tag The user tag to associate
*/
void ILibWebServer_SetTag(ILibWebServer_ServerToken object, void *Tag)
{
	struct ILibWebServer_StateModule *s = (struct ILibWebServer_StateModule*)object;
	s->Tag = Tag;
}

/*! \fn ILibWebServer_GetTag(ILibWebServer_ServerToken object)
\brief Gets the user tag associated with the server
\param object The ILibWebServer to query
\returns The associated user tag
*/
void *ILibWebServer_GetTag(ILibWebServer_ServerToken object)
{
	struct ILibWebServer_StateModule *s = (struct ILibWebServer_StateModule*)object;
	return(s->Tag);
}

//
// Internal method dispatched by a timer to idle out a session
//
// A session can idle in two ways. 
// 1.) A TCP connection is established, but a request isn't received within an allotted time period
// 2.) A request is answered, and another request isn't received with an allotted time period
// 
void ILibWebServer_IdleSink(void *object)
{
	struct ILibWebServer_Session *session = (struct ILibWebServer_Session*)object;
	if (ILibAsyncSocket_IsFree(session->Reserved2) == 0)
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(session->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "WebSession[%p] IDLE/Timeout", (void*)session);

		// This is OK, because we're on the MicroStackThread
		ILibAsyncServerSocket_Disconnect(session->Reserved1, session->Reserved2);
	}
}


//
// Chain Destroy handler
//
void ILibWebServer_Destroy(void *object)
{
	struct ILibWebServer_StateModule *s = (struct ILibWebServer_StateModule*)object;
	void *en;
	void *data;
	char *key;
	int keyLength;

	if (s->VirtualDirectoryTable != NULL)
	{
		//
		// If there are registered Virtual Directories, we need to free the resources
		// associated with them
		//
		en = ILibHashTree_GetEnumerator(s->VirtualDirectoryTable);
		while (ILibHashTree_MoveNext(en) == 0)
		{
			ILibHashTree_GetValue(en, &key, &keyLength, &data);
			free(data);
		}
		ILibHashTree_DestroyEnumerator(en);
		ILibDestroyHashTree(s->VirtualDirectoryTable);
	}
}
//
// Internal method dispatched from the underlying WebClient engine
//
// <param name="WebReaderToken">The WebClient token</param>
// <param name="InterruptFlag">Flag indicating session was interrupted</param>
// <param name="header">The HTTP header structure</param>
// <param name="bodyBuffer">buffer pointing to HTTP body</param>
// <param name="beginPointer">buffer pointer offset</param>
// <param name="endPointer">buffer length</param>
// <param name="done">Flag indicating if the entire packet has been read</param>
// <param name="user1"></param>
// <param name="user2">The ILibWebServer uses this to pass the ILibWebServer_Session object</param>
// <param name="PAUSE">Flag to pause data reads on the underlying WebClient engine</param>
void ILibWebServer_OnResponse(
	void *WebReaderToken,
	int InterruptFlag,
struct packetheader *header,
	char *bodyBuffer,
	int *beginPointer,
	int endPointer,
	ILibWebClient_ReceiveStatus recvStatus,
	void *user1,
	void *user2,
	int *PAUSE)
{
	struct ILibWebServer_Session *ws = (struct ILibWebServer_Session*)user2;
	struct ILibWebServer_StateModule *wsm = (struct ILibWebServer_StateModule*)ws->Parent;

	char *tmp;
	int tmpLength;
	struct parser_result *pr;
	int PreSlash = 0;

	UNREFERENCED_PARAMETER(WebReaderToken);
	UNREFERENCED_PARAMETER(user1);

	if (header == NULL) return;

	ws->buffer = bodyBuffer + *beginPointer;
	ws->bufferLength = endPointer - *beginPointer;
	ws->done = recvStatus;

#if defined(MAX_HTTP_HEADER_SIZE) || defined(MAX_HTTP_PACKET_SIZE)
	if (ws->done == (int)ILibWebClient_DoneCode_HeaderTooBig ||
		ws->done == (int)ILibWebClient_DoneCode_BodyTooBig)
	{
		//
		// We need to return a 413 Error code for this condition, to be nice
		//
		{
			char body[255];
			int bodyLength;
			bodyLength = snprintf(body, 255, "HTTP/1.1 413 Request Too Big (MaxHeader=%d)\r\n\r\n", MAX_HTTP_HEADER_SIZE);
			ILibAsyncSocket_Send(ws->Reserved2, body, bodyLength, ILibAsyncSocket_MemoryOwnership_USER);
		}
	}
#endif
	//
	// Reserved4 = Request Answered Flag
	//	If this flag is set, the request was answered
	// Reserved5 = Request Made Flag
	//	If this flag is set, a request has been received
	//
	if (ws->Reserved4 != 0 || ws->Reserved5 == 0)
	{
		//
		// This session is no longer idle
		//
		ws->Reserved4 = 0;
		ws->Reserved5 = 1;
		ws->Reserved8 = 0;
		ILibLifeTime_Remove(((struct ILibWebServer_StateModule*)ws->Parent)->LifeTime, ws);
	}

	//
	// Check to make sure that the request contains a host header, as required
	// by RFC-2616, for HTTP/1.1 requests
	//
	if (recvStatus == ILibWebClient_ReceiveStatus_Complete && header != NULL && header->Directive != NULL && atof(header->Version) > 1)
	{
		if (ILibGetHeaderLine(header, "host", 4) == NULL)
		{
			//
			// Host header is missing
			//
			char body[255];
			int bodyLength;
			bodyLength = snprintf(body, 255, "HTTP/1.1 400 Bad Request (Missing Host Field)\r\n\r\n");
			ILibWebServer_Send_Raw(ws, body, bodyLength, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_Done);
			return;
		}
	}

	if (recvStatus == ILibWebClient_ReceiveStatus_Complete && header != NULL && header->Directive != NULL)
	{
		// Check to see if this is a WebSocket request, so we can modify a flag
		ws->Reserved16 = (ILibGetHeaderLine(header, "Sec-WebSocket-Key", 17) != NULL) ? ILibWebServer_WebSocket_DataType_REQUEST : ILibWebServer_WebSocket_DataType_UNKNOWN;
	}

	//
	// Check Virtual Directory
	//
	if (wsm->VirtualDirectoryTable != NULL)
	{
		//
		// Reserved7 = Virtual Directory State Object
		//
		if (ws->Reserved7 == NULL)
		{
			//
			// See if we can find the virtual directory.
			// If we do, set the State Object, so future responses don't need to 
			// do it again
			//
			pr = ILibParseString(header->DirectiveObj, 0, header->DirectiveObjLength, "/", 1);
			if (pr->FirstResult->datalength == 0)
			{
				// Does not start with '/'
				tmp = pr->FirstResult->NextResult->data;
				tmpLength = pr->FirstResult->NextResult->datalength;
				PreSlash = 1;
			}
			else
			{
				// Starts with '/'
				tmp = pr->FirstResult->data;
				tmpLength = pr->FirstResult->datalength;
			}
			ILibDestructParserResults(pr);
			//
			// Does the Virtual Directory Exist?
			//
			if (ILibHasEntry(wsm->VirtualDirectoryTable, tmp, tmpLength) != 0)
			{
				//
				// Virtual Directory is defined
				//
				header->Reserved = header->DirectiveObj;
				header->DirectiveObj = tmp + tmpLength;
				header->DirectiveObjLength -= (tmpLength + PreSlash);
				//
				// Set the StateObject, then call the handler
				//
				ws->Reserved7 = ILibGetEntry(wsm->VirtualDirectoryTable, tmp, tmpLength);
				if (ws->Reserved7 != NULL) ((ILibWebServer_VirtualDirectory)((struct ILibWebServer_VirDir_Data*)ws->Reserved7)->callback)(ws, header, bodyBuffer, beginPointer, endPointer, recvStatus, ((struct ILibWebServer_VirDir_Data*)ws->Reserved7)->user);
			}
			else if (ws->OnReceive != NULL)
			{
				//
				// If the virtual directory doesn't exist, just call the main handler
				//
				ws->OnReceive(ws, InterruptFlag, header, bodyBuffer, beginPointer, endPointer, (ILibWebServer_DoneFlag)recvStatus);
			}
		}
		else
		{
			if (ws->Reserved13 == 0)
			{
				//
				// The state object was already set, so we know this is the handler to use. So easy!
				//
				((ILibWebServer_VirtualDirectory)((struct ILibWebServer_VirDir_Data*)ws->Reserved7)->callback)(ws, header, bodyBuffer, beginPointer, endPointer, recvStatus, ((struct ILibWebServer_VirDir_Data*)ws->Reserved7)->user);
			}
			else
			{
				ws->OnReceive(ws, InterruptFlag, header, bodyBuffer, beginPointer, endPointer, (ILibWebServer_DoneFlag)recvStatus);
			}
		}
	}
	else if (ws->OnReceive != NULL)
	{
		//
		// Since there is no Virtual Directory lookup table, none were registered,
		// so we know we have no choice but to call the regular handler
		//
		ILibRemoteLogging_printf(ILibChainGetLogger(ws->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "WebSession[%p] << %s [%s] %s", (void*)ws, ILibRemoteLogging_ConvertAddress((struct sockaddr*)header->Source), header->Directive, header->DirectiveObj);

		ws->OnReceive(ws, InterruptFlag, header, bodyBuffer, beginPointer, endPointer, (ILibWebServer_DoneFlag)recvStatus);
	}


	//
	// Reserved8 = RequestAnswered method has been called
	//
	if (recvStatus == ILibWebClient_ReceiveStatus_Complete && InterruptFlag == 0 && header != NULL && ws->Reserved8 == 0)
	{
		//
		// The request hasn't been satisfied yet, so stop reading from the socket until it is
		//
		*PAUSE = 1;
	}
}

//
// Internal method dispatched from the underlying ILibAsyncServerSocket module
//
// This is dispatched when the underlying buffer has been reallocated, which may
// neccesitate extra processing
// <param name="AsyncServerSocketToken">AsyncServerSocket token</param>
// <param name="ConnectionToken">Connection token (Underlying ILibAsyncSocket)</param>
// <param name="user">The ILibWebServer_Session object</param>
// <param name="offSet">Offset to the new buffer location</param>
void ILibWebServer_OnBufferReAllocated(void *AsyncServerSocketToken, void *ConnectionToken, void *user, ptrdiff_t offSet)
{
	struct ILibWebServer_Session *ws = (struct ILibWebServer_Session*)user;

	UNREFERENCED_PARAMETER(AsyncServerSocketToken);
	UNREFERENCED_PARAMETER(ConnectionToken);

	//
	// We need to pass this down to our internal ILibWebClient for further processing
	// Reserved2 = ConnectionToken
	// Reserved3 = WebClientDataObject
	//
	ILibWebClient_OnBufferReAllocate(ws->Reserved2, ws->Reserved3, offSet);
}
#if defined(MAX_HTTP_PACKET_SIZE)
void ILibWebServer_OnBufferSizeExceeded(void *connectionToken, void *user)
{
	//
	// We need to return a 413 Error code for this condition, to be nice
	//

	char body[255];
	int bodyLength;
	bodyLength = sprintf(body, "HTTP/1.1 413 Request Too Big (MaxPacketSize=%d)\r\n\r\n", MAX_HTTP_PACKET_SIZE);
	ILibAsyncSocket_Send(connectionToken, body, bodyLength, ILibAsyncSocket_MemoryOwnership_USER);
}
#endif
//
// Internal method dispatched from the underlying ILibAsyncServerSocket module
//
// <param name="AsyncServerSocketModule">AsyncServerSocket token</param>
// <param name="ConnectionToken">Connection token (Underlying ILibAsyncSocket)</param>
// <param name="user">User object that can be set. (used here for ILibWebServer_Session</param>
void ILibWebServer_OnConnect(void *AsyncServerSocketModule, void *ConnectionToken, void **user)
{
	struct ILibWebServer_StateModule *wsm;
	struct ILibWebServer_Session *ws;

	//
	// Create a new ILibWebServer_Session to represent this connection
	//
	wsm = (struct ILibWebServer_StateModule*)ILibAsyncServerSocket_GetTag(AsyncServerSocketModule);
	if ((ws = (struct ILibWebServer_Session*)malloc(sizeof(struct ILibWebServer_Session))) == NULL) ILIBCRITICALEXIT(254);
	memset(ws, 0, sizeof(struct ILibWebServer_Session));

	ws->Reserved_Chain = wsm->Chain;
	ws->Reserved_Flags = (unsigned int)ILibTransports_WebServer;
	ws->closePtr = (ILibTransport_ClosePtr)&ILibWebServer_DisconnectSession;
	ws->sendPtr = (ILibTransport_SendPtr)&ILibWebServer_Send_Raw;
	ws->pendingPtr = (ILibTransport_PendingBytesToSendPtr)&ILibWebServer_Session_GetPendingBytesToSend;

	sem_init(&(ws->Reserved11), 0, 1); // Initialize the SessionLock
	ws->Reserved12 = 1; // Reference Counter, Initial count should be 1

	//printf("#### ALLOCATED (%d) ####\r\n", ConnectionToken);

	ws->Parent = wsm;
	ws->Reserved1 = AsyncServerSocketModule;
	ws->Reserved2 = ConnectionToken;
	ws->Reserved3 = ILibCreateWebClientEx(&ILibWebServer_OnResponse, ConnectionToken, wsm, ws);
	ws->User = wsm->User;
	*user = ws;
#if defined(MAX_HTTP_PACKET_SIZE)
	ILibAsyncSocket_SetMaximumBufferSize(ConnectionToken, MAX_HTTP_PACKET_SIZE, &ILibWebServer_OnBufferSizeExceeded, ws);
#endif	
	//
	// We want to know when this connection reallocates its internal buffer, because we may
	// need to fix a few things
	//
	ILibAsyncServerSocket_SetReAllocateNotificationCallback(AsyncServerSocketModule, ConnectionToken, &ILibWebServer_OnBufferReAllocated);

	//
	// Add a timed callback, because if we don't receive a request within a specified
	// amount of time, we want to close the socket, so we don't waste resources
	//
	ILibLifeTime_Add(wsm->LifeTime, ws, HTTP_SESSION_IDLE_TIMEOUT, &ILibWebServer_IdleSink, NULL);

	SESSION_TRACK(ws, "* Allocated *");
	SESSION_TRACK(ws, "AddRef");

	ILibRemoteLogging_printf(ILibChainGetLogger(ws->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "WebServer: OnSession[%p]", (void*)ws);

	//
	// Inform the user that a new session was established
	//
	if (wsm->OnSession != NULL) wsm->OnSession(ws, wsm->User);
}

//
// Internal method dispatched from the underlying AsyncServerSocket engine
// 
// <param name="AsyncServerSocketModule">The ILibAsyncServerSocket token</param>
// <param name="ConnectionToken">The ILibAsyncSocket connection token</param>
// <param name="user">The ILibWebServer_Session object</param>
void ILibWebServer_OnDisconnect(void *AsyncServerSocketModule, void *ConnectionToken, void *user)
{
	struct ILibWebServer_Session *ws = (struct ILibWebServer_Session*)user;

	UNREFERENCED_PARAMETER(AsyncServerSocketModule);
	UNREFERENCED_PARAMETER(ConnectionToken);

#ifdef _DEBUG
	//printf("#### RELEASE (%d) ####\r\n", ConnectionToken);
	if (ws == NULL) { PRINTERROR(); ILIBCRITICALEXIT(253); }
#endif

	//
	// If this was a non-persistent connection, the response will queue this up to be called.
	// Some clients may close the socket anyway, before the server does, so we should remove this
	// so we don't accidently close the socket again.
	//
	ILibLifeTime_Remove(((struct ILibWebServer_StateModule*)ws->Parent)->LifeTime, ws->Reserved2);
	if (ws->Reserved10 != NULL) ws->Reserved10 = NULL;

	//
	// Reserved4 = RequestAnsweredFlag
	// Reserved5 = RequestMadeFlag
	//
	if (ws->Reserved4 != 0 || ws->Reserved5 == 0)
	{
		ILibLifeTime_Remove(((struct ILibWebServer_StateModule*)ws->Parent)->LifeTime, ws);
		ws->Reserved4 = 0;
	}

	SESSION_TRACK(ws, "OnDisconnect");

	if (ws->Reserved_WebSocket_Request != NULL && ws->OnReceive != NULL)
	{
		int tmp_ptr = 0;
		ws->OnReceive(ws, 0, (struct packetheader*)ws->Reserved_WebSocket_Request, NULL, &tmp_ptr, 0, ILibWebServer_DoneFlag_Done);
	}

	ILibRemoteLogging_printf(ILibChainGetLogger(ws->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "WebServer: OnDisconnect[%p]", (void*)ws);

	//
	// Notify the user that this session disconnected
	//	
	if (ws->OnDisconnect != NULL) ws->OnDisconnect(ws);

	ILibWebServer_Release((struct ILibWebServer_Session *)user);
}

int ILibWebServer_ProcessWebSocketData(struct ILibWebServer_Session *ws, char* buffer, int offset, int length)
{
	int x;
	int i = offset + 2;
	int plen;
	unsigned short hdr;
	char* maskingKey = NULL;
	int FIN;
	unsigned char OPCODE;
	int tempBegin = 0;

	if (length < 2) { return(offset); } // We need at least 2 bytes to read enough of the headers to know how long the frame is

	hdr = ntohs(((unsigned short*)(buffer + offset))[0]);
	FIN = (hdr & WEBSOCKET_FIN) != 0;
	OPCODE = (hdr & WEBSOCKET_OPCODE) >> 8;

	plen = (unsigned char)(hdr & WEBSOCKET_PLEN);
	if (plen == 126)
	{
		if (length < 4) { return(offset); } // We need at least 4 bytes to read enough of the headers
		plen = (unsigned short)ntohs(((unsigned short*)(buffer + offset))[1]);
		i += 2;
	}
	else if (plen == 127)
	{
		if (length < 10) 
		{ 
			return(offset); // We need at least 10 bytes to read enough of the headers
		} 
		else
		{
			unsigned long long v = ILibNTOHLL(((unsigned long long*)(buffer + offset + 2))[0]);
			if(v > 0x7FFFFFFFUL)
			{
				// this value is too big to store in a 32 bit signed variable, so disconnect the websocket.
				ILibWebServer_DisconnectSession(ws);
				return(length);
			}
			else
			{
				// this value can be represented with a signed 32 bit variable
				plen = (int)v;
				i += 8;
			}
		}
	}

	if (length < (i + plen + ((unsigned char)(hdr & WEBSOCKET_MASK) != 0 ? 4 : 0)))
	{
		return(offset); // Don't have the entire packet
	}

	maskingKey = ((unsigned char)(hdr & WEBSOCKET_MASK) == 0) ? NULL : (buffer + i);
	if (maskingKey != NULL)
	{
		// Unmask the data
		i += 4;	// Move ptr to start of data

		for (x = 0; x < plen; ++x)
		{
			buffer[i + x] = buffer[i + x] ^ maskingKey[x % 4];
		}
	}

	if (ws->OnReceive == NULL) { return (i + plen); } // If there is no receiver, then just return after we consume everything

	if (OPCODE < 0x8)
	{
		// NON-CONTROL OP-CODE
		if (ws->Reserved20 == 0)
		{
			// We will just pass the data up, and let the app handle fragment re-assembly
			ws->Reserved16 = (int)OPCODE;
			tempBegin = 0;
			ws->OnReceive(ws, 0, (struct packetheader*)ws->Reserved_WebSocket_Request, buffer + i, &tempBegin, plen, FIN == 0 ? ILibWebServer_DoneFlag_Partial : ILibWebServer_DoneFlag_LastPartial);
		}
		else
		{
			// We will try to automatically re-assemble fragments, up to the max buffer size the user specified
			if (OPCODE != 0) { ws->Reserved16 = (int)OPCODE; } // Set the DataFrame Type, so the user can query it

			if (FIN != 0 && ws->Reserved18 == 0 && ws->Reserved21 == 0)
			{
				// We have an entire fragment, and we didn't save any of it yet... We can just forward it up without copying the buffer
				tempBegin = 0;
				ws->OnReceive(ws, 0, (struct packetheader*)ws->Reserved_WebSocket_Request, buffer + i, &tempBegin, plen, ILibWebServer_DoneFlag_NotDone);
			}
			else
			{
				if (ws->Reserved18 + plen >= ws->Reserved19)
				{
					// Need to grow the buffer

					if (ws->Reserved19 == ws->Reserved20)
					{
						// We are already maxed out, so just send what we have as an unfinished fragment	
						ws->Reserved21 = 1; // Set this flag, becuase we can't reassemble, so our FIN flag will be different to reflect that					
						tempBegin = 0;
						ws->OnReceive(ws, 0, (struct packetheader*)ws->Reserved_WebSocket_Request, ws->Reserved17, &tempBegin, ws->Reserved18, ILibWebServer_DoneFlag_Partial);
						ws->Reserved18 = 0; // Reset the index, becuase new data is going to go to the front
					}
					else
					{
						// We can grow the buffer
						ws->Reserved19 = ws->Reserved19 * 2;
						if (ws->Reserved19 > ws->Reserved20) { ws->Reserved19 = ws->Reserved20; }
						if ((ws->Reserved17 = (char*)realloc(ws->Reserved17, ws->Reserved19)) == NULL) { ILIBCRITICALEXIT(254); }
					}
				}

				memcpy(ws->Reserved17 + ws->Reserved18, buffer + i, plen);
				ws->Reserved18 += plen;

				if (FIN != 0)
				{
					ws->OnReceive(ws, 0, (struct packetheader*)ws->Reserved_WebSocket_Request, ws->Reserved17, &tempBegin, ws->Reserved18, ws->Reserved21 == 0 ? ILibWebServer_DoneFlag_NotDone : ILibWebServer_DoneFlag_LastPartial);
					ws->Reserved21 = 0; // Reset (We can try to auto-assemble)
					ws->Reserved18 = 0; // Reset (We can write to the start of the buffer)
				}
			}
		}
	}
	else
	{
		// CONTROL
		switch (OPCODE)
		{
		case WEBSOCKET_OPCODE_CLOSE:
		{
			int sendResponse = 0;
			// CONNECTION-CLOSE
			sem_wait(&(ws->Reserved11));
			if (ws->Reserved22 == 0)	{ ws->Reserved22 = 1; sendResponse = 1; }
			sem_post(&(ws->Reserved11));

			if (sendResponse != 0)
			{
				// Echo the Close Frame
				ILibWebServer_WebSocket_Send(ws, buffer + i, plen > 0 ? 2 : 0, (ILibWebServer_WebSocket_DataTypes)WEBSOCKET_OPCODE_CLOSE, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_WebSocket_FragmentFlag_Complete);
			}
			ILibRemoteLogging_printf(ILibChainGetLogger(ws->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "ILibWebServer: << [Close OP-Code] on %p", (void*)ws);
			ILibWebServer_DisconnectSession(ws);
		}
			break;
		case WEBSOCKET_OPCODE_PING:
			// PING, respond with a PONG
			ILibWebServer_WebSocket_Send(ws, NULL, 0, (ILibWebServer_WebSocket_DataTypes)WEBSOCKET_OPCODE_PONG, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_WebSocket_FragmentFlag_Complete);
			break;
		case WEBSOCKET_OPCODE_PONG:
			// PONG

			break;
		}
	}



	return(i + plen);
}

void ILibWebServer_WebSocket_Close(struct ILibWebServer_Session *ws)
{
	int sent = 0;
	unsigned short code = htons(1000); // Normal Closure

	sem_wait(&(ws->Reserved11));
	if (ws->Reserved22 == 0)	{ ws->Reserved22 = 1; sent = 1; }
	sem_post(&(ws->Reserved11));

	if (sent != 0)
	{
		ILibWebServer_WebSocket_Send(ws, (char*)&code, 2, (ILibWebServer_WebSocket_DataTypes)WEBSOCKET_OPCODE_CLOSE, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_WebSocket_FragmentFlag_Complete);
	}
}

//
// Internal method dispatched from the underlying ILibAsyncServerSocket engine
//
// <param name="AsyncServerSocketModule">The ILibAsyncServerSocket token</param>
// <param name="ConnectionToken">The ILibAsyncSocket connection token</param>
// <param name="buffer">The receive buffer</param>
// <param name="p_beginPointer">buffer offset</param>
// <param name="endPointer">buffer length</param>
// <param name="OnInterrupt">Function Pointer to handle Interrupts</param>
// <param name="user">ILibWebServer_Session object</param>
// <param name="PAUSE">Flag to pause data reads on the underlying AsyncSocket engine</param>
void ILibWebServer_OnReceive(void *AsyncServerSocketModule, void *ConnectionToken, char* buffer, int *p_beginPointer, int endPointer, void(**OnInterrupt)(void *AsyncServerSocketMoudle, void *ConnectionToken, void *user), void **user, int *PAUSE)
{
	//
	// Pipe the data down to our internal WebClient engine, which will do
	// all the HTTP processing
	//
	struct ILibWebServer_Session *ws = (struct ILibWebServer_Session*)(*user);

	UNREFERENCED_PARAMETER(AsyncServerSocketModule);
	UNREFERENCED_PARAMETER(OnInterrupt);

	if (ws->Reserved_WebSocket_Request == NULL)
	{
		// Normal HTTP Processing Rules
		ILibWebClient_OnData(ConnectionToken, buffer, p_beginPointer, endPointer, NULL, &(ws->Reserved3), PAUSE);
	}
	else
	{
		// Websocket Processing Rules
		*p_beginPointer = ILibWebServer_ProcessWebSocketData(ws, buffer, *p_beginPointer, endPointer);
	}
}

//
// Internal method dispatched from the underlying ILibAsyncServerSocket engine, signaling an interrupt
//
// <param name="AsyncServerSocketModule">The ILibAsyncServerSocket token</param>
// <param name="ConnectionToken">The ILibAsyncSocket connection token</param>
// <param name="user">The ILibWebServer_Session object</param>
void ILibWebServer_OnInterrupt(void *AsyncServerSocketModule, void *ConnectionToken, void *user)
{
	struct ILibWebServer_Session *session = (struct ILibWebServer_Session*)user;
	if (session == NULL) return;
	session->SessionInterrupted = 1;

	UNREFERENCED_PARAMETER(AsyncServerSocketModule);
	UNREFERENCED_PARAMETER(ConnectionToken);

	// This is ok, because this is MicroStackThread
	ILibWebClient_DestroyWebClientDataObject(session->Reserved3);
}

//
// Internal method called when a request has been answered. Dispatched from Send routines
//
// <param name="session">The ILibWebServer_Session object</param>
// <returns>Flag indicating if the session was closed</returns>
enum ILibWebServer_Status ILibWebServer_RequestAnswered(struct ILibWebServer_Session *session)
{
	struct packetheader *hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	struct packetheader_field_node *f;
	int PersistentConnection = 0;

	ILibRemoteLogging_printf(ILibChainGetLogger(session->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "WebSession[%p] (Request Answered)", (void*)session);

	if (hdr == NULL) return ILibWebServer_ALL_DATA_SENT;

	//
	// Reserved7 = Virtual Directory State Object
	//	We delete this, because the request is finished, so we don't need to direct
	//	data to this handler anymore. It needs to be recalculated next time
	//
	session->Reserved7 = NULL;

	//
	// Reserved8 = RequestAnswered method called
	//If this is set, this method was already called, so we can just exit
	//
	if (session->Reserved8 != 0) return ILibWebServer_ALL_DATA_SENT;

	//
	// Set the flags, so if this re-enters, we don't process this again
	//
	session->Reserved8 = 1;
	f = hdr->FirstField;

	//
	// Reserved5 = Request Made. Since the request was answered, we can clear this.
	//
	session->Reserved5 = 0;

	//
	// Reserved6 = CloseOverrideFlag
	//	which means the session must be closed when request is complete
	//
	if (session->Reserved6 == 0)
	{
		//{{{ REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT--> }}}
		if (hdr->VersionLength == 3 && memcmp(hdr->Version, "1.0", 3) != 0)
		{
			// HTTP 1.1+ , Check for CLOSE token
			PersistentConnection = 1;
			while (f != NULL)
			{
				if (f->FieldLength == 10 && strncasecmp(f->Field, "CONNECTION", 10) == 0)
				{
					if (f->FieldDataLength == 5 && strncasecmp(f->FieldData, "CLOSE", 5) == 0)
					{
						PersistentConnection = 0;
						break;
					}
				}
				f = f->NextField;
			}
		}
		//{{{ <--REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT }}}
	}

	if (PersistentConnection == 0)
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(session->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "...WebSession[%p] is NON-PERSISTENT", (void*)session);
		//
		// Ensure calling on MicroStackThread. This will just result dispatching the callback on
		// the microstack thread
		//
		ILibLifeTime_Add(((struct ILibWebServer_StateModule*)session->Parent)->LifeTime, session->Reserved2, 0, &ILibAsyncSocket_Disconnect, NULL);
	}
	else
	{
		ILibRemoteLogging_printf(ILibChainGetLogger(session->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "...WebSession[%p] is PERSISTENT", (void*)session);
		//
		// This is a persistent connection. Set a timed callback, to idle this session if necessary
		//
		ILibLifeTime_Add(((struct ILibWebServer_StateModule*)session->Parent)->LifeTime, session, HTTP_SESSION_IDLE_TIMEOUT, &ILibWebServer_IdleSink, NULL);
		ILibWebClient_FinishedResponse_Server(session->Reserved3);
		//
		// Since we're done with this request, resume the underlying socket, so we can continue
		//
		ILibWebClient_Resume(session->Reserved3);
	}
	return (PersistentConnection == 0 ? ILibWebServer_SEND_RESULTED_IN_DISCONNECT : ILibWebServer_ALL_DATA_SENT);
}

//
// Internal method dispatched from the underlying ILibAsyncServerSocket engine
//
// <param name="AsyncServerSocketModule">The ILibAsyncServerSocket token</param>
// <param name="ConnectionToken">The ILibAsyncSocket connection token</param>
// <param name="user">The ILibWebServer_Session object</param>
void ILibWebServer_OnSendOK(void *AsyncServerSocketModule, void *ConnectionToken, void *user)
{
	int flag = 0;
	struct ILibWebServer_Session *session = (struct ILibWebServer_Session*)user;

	UNREFERENCED_PARAMETER(AsyncServerSocketModule);
	UNREFERENCED_PARAMETER(ConnectionToken);

	//
	// Reserved4 = RequestAnsweredFlag
	//
	if (session->Reserved4 != 0)
	{
		// This is normally called when the response was sent. But since it couldn't get through
		// the first time, this method gets dispatched when it did, so now we have to call it.
		flag = ILibWebServer_RequestAnswered(session);
	}
	if (session->OnSendOK != NULL && flag != ILibWebServer_SEND_RESULTED_IN_DISCONNECT)
	{
		// Pass this event on, if everything is ok
		session->OnSendOK(session);
	}
}

#ifndef MICROSTACK_NOTLS
#ifdef MICROSTACK_TLS_DETECT
void ILibWebServer_SetTLS(ILibWebServer_ServerToken object, void *ssl_ctx, int enableTLSDetect)
{
	struct ILibWebServer_StateModule *module = (struct ILibWebServer_StateModule*)object;
	ILibAsyncServerSocket_SetSSL_CTX(module->ServerSocket, ssl_ctx, enableTLSDetect);
}
#else
void ILibWebServer_SetTLS(ILibWebServer_ServerToken object, void *ssl_ctx)
{
	struct ILibWebServer_StateModule *module = (struct ILibWebServer_StateModule*)object;
	ILibAsyncServerSocket_SetSSL_CTX(module->ServerSocket, ssl_ctx);
}
#endif
#endif

/*! \fn ILibWebServer_Create(void *Chain, int MaxConnections, int PortNumber,ILibWebServer_Session_OnSession OnSession, void *User)
\brief Constructor for ILibWebServer
\param Chain The Chain to add this module to. (Chain must <B>not</B> be running)
\param MaxConnections The maximum number of simultaneous connections
\param PortNumber The Port number to listen to (0 = Random)
\param OnSession Function Pointer to dispatch on when new Sessions are established
\param User User state object to pass to OnSession
*/
ILibWebServer_ServerToken ILibWebServer_CreateEx(void *Chain, int MaxConnections, unsigned short PortNumber, int loopbackFlag, ILibWebServer_Session_OnSession OnSession, void *User)
{
	struct ILibWebServer_StateModule *RetVal = (struct ILibWebServer_StateModule*)malloc(sizeof(struct ILibWebServer_StateModule));

	if (RetVal == NULL) { PRINTERROR(); return NULL; }
	memset(RetVal, 0, sizeof(struct ILibWebServer_StateModule));
	RetVal->Destroy = &ILibWebServer_Destroy;
	RetVal->Chain = Chain;
	RetVal->OnSession = OnSession;
	RetVal->DigestAuth_NonceDuration = DIGEST_AUTHENTICATION_NONCE_DEFAULT_DURATION_MINUTES;

	//
	// Create the underling ILibAsyncServerSocket
	//
	RetVal->ServerSocket = ILibCreateAsyncServerSocketModule(
		Chain,
		MaxConnections,
		PortNumber,
		INITIAL_BUFFER_SIZE,
		loopbackFlag,
		&ILibWebServer_OnConnect,			// OnConnect
		&ILibWebServer_OnDisconnect,		// OnDisconnect
		&ILibWebServer_OnReceive,			// OnReceive
		&ILibWebServer_OnInterrupt,			// OnInterrupt
		&ILibWebServer_OnSendOK				// OnSendOK
		);

	if (RetVal->ServerSocket == NULL) { free(RetVal); return NULL; }

	//
	// Set ourselves in the User tag of the underlying ILibAsyncServerSocket
	//
	ILibAsyncServerSocket_SetTag(RetVal->ServerSocket, RetVal);
	RetVal->LifeTime = ILibGetBaseTimer(Chain); //ILibCreateLifeTime(Chain);
	RetVal->User = User;
	ILibAddToChain(Chain, RetVal);
	return RetVal;
}

/*! \fn ILibWebServer_GetPortNumber(ILibWebServer_ServerToken WebServerToken)
\brief Returns the port number that this module is listening to
\param WebServerToken The ILibWebServer to query
\returns The listening port number
*/
unsigned short ILibWebServer_GetPortNumber(ILibWebServer_ServerToken WebServerToken)
{
	struct ILibWebServer_StateModule *WSM = (struct ILibWebServer_StateModule*) WebServerToken;
	return ILibAsyncServerSocket_GetPortNumber(WSM->ServerSocket);
}

void ILibWebServer_Digest_CalculateNonce(struct ILibWebServer_Session *session, long long expiration, char* buffer)
{
	char temp[33];
	if (expiration == 0)
	{
		char tmp[8];
		util_hexToBuf((char*)ILibGetEntry(session->Reserved_DigestTable, "opaque", 6), 16, tmp);
		expiration = ((long long*)tmp)[0];
	}
	memcpy(temp, &expiration, 8);
	memcpy(temp + 8, &(session->Parent), sizeof(void*));

	util_md5hex(temp, 8 + sizeof(void*), buffer);
}

void ILibWebServer_Digest_SendUnauthorized(struct ILibWebServer_Session *session, char* realm, int realmLen, char* html, int htmllen)
{
	long long nonceExpiration = ILibGetUptime() + (long long)(((struct ILibWebServer_StateModule*)session->Parent)->DigestAuth_NonceDuration * 60000);
	char nonce[33];
	char opaque[17];
	int l;

	if (realm == NULL)
	{
		realm = (char*)ILibGetEntry(session->Reserved_DigestTable, "realm", 5);
		if (realm != NULL)
		{
			realmLen = strlen(realm);
		}
		else
		{
			return;
		}
	}

	ILibWebServer_Digest_CalculateNonce(session, nonceExpiration, nonce);
	util_tohex((char*)&nonceExpiration, 8, opaque);

	ILibWebServer_Send_Raw(session, "HTTP/1.1 401 Unauthorized\r\nContent-Type: text/html\r\nWWW-Authenticate: Digest realm=\"", 84, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_NotDone);
	ILibWebServer_Send_Raw(session, realm, realmLen, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_NotDone);
	ILibWebServer_Send_Raw(session, "\", nonce=\"", 10, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_NotDone);
	ILibWebServer_Send_Raw(session, nonce, 32, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_NotDone);
	ILibWebServer_Send_Raw(session, "\", opaque=\"", 11, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_NotDone);
	ILibWebServer_Send_Raw(session, opaque, 16, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_NotDone);
	l = snprintf(nonce, 33, "\"\r\nContent-Length: %d\r\n\r\n", htmllen);
	ILibWebServer_Send_Raw(session, nonce, l, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_NotDone);
	ILibWebServer_Send_Raw(session, html, htmllen, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_Done);
}


int ILibWebServer_Digest_ParseAuthenticationHeader2(char* value, int valueLen, char **token, char **tokenValue)
{
	int len;
	int i = ILibString_IndexOf(value, valueLen, "=", 1);
	if (i < 0) { return(1); }

	*token = value;

	len = ILibTrimString(token, i);
	(*token)[len] = 0;

	*tokenValue = value + i + 1;
	len = valueLen - (i + 1);

	len = ILibTrimString(tokenValue, len);
	if ((*tokenValue)[0] == '\'' || (*tokenValue)[0] == '\"')
	{
		*tokenValue += 1;
		len -= 1;
	}
	if ((*tokenValue)[len - 1] == '\'' || (*tokenValue)[len - 1] == '\"')
	{
		len -= 1;
	}
	(*tokenValue)[len] = 0;
	return(0);
}
void ILibWebServer_Digest_ParseAuthenticationHeader(void* table, char* value, int valueLen)
{
	struct parser_result *pr = ILibParseStringAdv(value, 7, valueLen - 7, ",", 1);
	struct parser_result_field *f = pr->FirstResult;

	while (f != NULL)
	{
		char *token;
		char* tokenValue;
		if (ILibWebServer_Digest_ParseAuthenticationHeader2(f->data, f->datalength, &token, &tokenValue) == 0)
		{
			ILibAddEntry(table, token, strlen(token), tokenValue);
		}
		f = f->NextResult;
	}

	ILibDestructParserResults(pr);
}

int ILibWebServer_Digest_IsCorrectRealmAndNonce(struct ILibWebServer_Session *session, char* realm, int realmLen)
{
	struct packetheader *hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	char* auth;
	char* userRealm = NULL;
	char* nonce = NULL;
	char* opaque = NULL;
	long long current;
	char expiration[8];
	char calculatedNonce[33];

	if (hdr == NULL) { return(0); }
	current = ILibGetUptime();


	if (session->Reserved_DigestTable != NULL) { ILibDestroyHashTree(session->Reserved_DigestTable); }
	session->Reserved_DigestTable = ILibInitHashTree_CaseInSensitive();
	auth = ILibGetHeaderLine(hdr, "Authorization", 13);

	ILibWebServer_Digest_ParseAuthenticationHeader(session->Reserved_DigestTable, auth, strlen(auth));
	userRealm = (char*)ILibGetEntry(session->Reserved_DigestTable, "realm", 5);
	nonce = (char*)ILibGetEntry(session->Reserved_DigestTable, "nonce", 5);
	opaque = (char*)ILibGetEntry(session->Reserved_DigestTable, "opaque", 6);

	if (opaque != NULL && userRealm != NULL && (int)strlen(userRealm) == realmLen && strncmp(userRealm, realm, realmLen) == 0)
	{
		// Realm is correct, now check the Nonce & Opaque Values
		if (strlen(opaque) != 16) { return(0); } // Invalid Opaque Block

		util_hexToBuf(opaque, 16, expiration);
		if (((long long*)expiration)[0] < current) { return(0); } // Opaque Block Expired

		ILibWebServer_Digest_CalculateNonce(session, ((long long*)expiration)[0], calculatedNonce);
		return(strcmp(nonce, calculatedNonce) == 0 ? 1 : 0);
	}
	else
	{
		return(0);
	}
}

// Returns true if authentication headers present
// NOTE: This call can only be made once per HTTP request, this call changes the HTTP headers.
int ILibWebServer_Digest_IsAuthenticated(struct ILibWebServer_Session *session, char* realm, int realmLen)
{
	struct packetheader *hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	char* auth;

	if (hdr == NULL) return 0;
	auth = ILibGetHeaderLine(hdr, "Authorization", 13);
	if (auth != NULL && ILibWebServer_Digest_IsCorrectRealmAndNonce(session, realm, realmLen) != 0) return 1;
	return 0;
}

char* ILibWebServer_Digest_GetUsername(struct ILibWebServer_Session *session)
{
	if (session->Reserved_DigestTable == NULL) return NULL;
	return((char*)ILibGetEntry(session->Reserved_DigestTable, "username", 8));
}

int ILibWebServer_Digest_ValidatePassword(struct ILibWebServer_Session *session, char* password, int passwordLen)
{
	int retVal, l;
	struct packetheader *hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	char nonce[33];
	char result1[33];
	char result2[33];
	char result3[33];
	char value[100];
	char *tmp;
	int tmpLen;

	char* username;
	char* realm;
	char* uri;
	char* response;

	username = (char*)ILibGetEntry(session->Reserved_DigestTable, "username", 8);
	realm = (char*)ILibGetEntry(session->Reserved_DigestTable, "realm", 5);
	uri = (char*)ILibGetEntry(session->Reserved_DigestTable, "uri", 3);
	response = (char*)ILibGetEntry(session->Reserved_DigestTable, "response", 8);

	if (username == NULL || uri == NULL || password == NULL || passwordLen == 0)
	{
		//ILibWebServer_Digest_SendUnauthorized(session, realm, strlen(realm));
		return 0;
	}

	ILibWebServer_Digest_CalculateNonce(session, 0, nonce);

	l = strlen(username) + strlen(realm) + passwordLen + 3;
	if ((tmp = (char*)malloc(l)) == NULL) ILIBCRITICALEXIT(254);
	tmpLen = snprintf(tmp, l, "%s:%s:", username, realm);
	memcpy(tmp + tmpLen, password, passwordLen);
	tmpLen += passwordLen;
	tmp[tmpLen] = 0;
	util_md5hex(tmp, tmpLen, result1);
	free(tmp);

	l = hdr->DirectiveLength + strlen(uri) + 2;
	if ((tmp = (char*)malloc(l)) == NULL) ILIBCRITICALEXIT(254);
	hdr->Directive[hdr->DirectiveLength] = 0;
	//ILibToLower(hdr->Directive, hdr->DirectiveLength, hdr->Directive);
	tmpLen = snprintf(tmp, l, "%s:%s", hdr->Directive, uri);
	tmp[tmpLen] = 0;
	util_md5hex(tmp, tmpLen, result2);

	free(tmp);
	tmpLen = snprintf(value, 100, "%s:%s:%s", result1, nonce, result2);
	value[tmpLen] = 0;
	util_md5hex(value, tmpLen, result3);

	retVal = strcmp(result3, response) == 0 ? 1 : 0;
	// if (retVal == 0) { ILibWebServer_Digest_SendUnauthorized(session, realm, strlen(realm)); }

	return retVal;
}

ILibTransport_DoneState ILibWebServer_WebSocket_TransportSend(void *transport, char* buffer, int bufferLength, ILibTransport_MemoryOwnership ownership, ILibTransport_DoneState done)
{
	return((ILibTransport_DoneState)ILibWebServer_WebSocket_Send((struct ILibWebServer_Session*)transport, buffer, bufferLength, ILibWebServer_WebSocket_DataType_BINARY, (enum ILibAsyncSocket_MemoryOwnership)ownership, (ILibWebServer_WebSocket_FragmentFlags)done));
}

char* ILibWebServer_IsCrossSiteRequest(ILibWebServer_Session* session)
{
	char* retVal = NULL;
	ILibHTTPPacket* hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	if (hdr != NULL)
	{
		char* host = ILibGetHeaderLine(hdr, "Host", 4);
		char* origin = ILibGetHeaderLine(hdr, "Origin", 6);

		if (host != NULL && origin != NULL)
		{
			host = ILibUrl_GetHost(host, -1);
			origin = ILibUrl_GetHost(origin, -1);

			if (strcmp(host, origin) != 0)
			{
				retVal = origin;
			}
		}
	}

	return(retVal);
}
int ILibWebServer_UpgradeWebSocket(struct ILibWebServer_Session *session, int autoFragmentReassemblyMaxBufferSize)
{
	SHA_CTX c;
	char shavalue[21];
	char* websocketKey;
	char* keyResult;
	int keyResultLen;
	struct packetheader *hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	if (hdr == NULL) { return(1); }

	session->Reserved20 = autoFragmentReassemblyMaxBufferSize;
	if (session->Reserved20 > 0)
	{
		session->Reserved19 = session->Reserved20 < 4096 ? session->Reserved20 : 4096;
		if ((session->Reserved17 = (char*)malloc(session->Reserved19)) == NULL) ILIBCRITICALEXIT(254);
	}

	websocketKey = ILibGetHeaderLine(hdr, "Sec-WebSocket-Key", 17);
	keyResult = ILibString_Cat(websocketKey, strlen(websocketKey), WEBSOCKET_GUID, strlen(WEBSOCKET_GUID));

	SHA1_Init(&c);
	SHA1_Update(&c, keyResult, strlen(keyResult));
	SHA1_Final((unsigned char*)shavalue, &c);
	shavalue[20] = 0;
	free(keyResult);
	keyResult = NULL;

	keyResultLen = ILibBase64Encode((unsigned char*)shavalue, 20, (unsigned char**)&keyResult);

	session->Reserved_WebSocket_Request = ILibClonePacket(hdr);

	ILibWebServer_Send_Raw(session, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ", 97, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_NotDone);
	ILibWebServer_Send_Raw(session, keyResult, keyResultLen, ILibAsyncSocket_MemoryOwnership_CHAIN, ILibWebServer_DoneFlag_NotDone);
	ILibWebServer_Send_Raw(session, "\r\n\r\n", 4, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_NotDone);

	session->Reserved8 = 1; // Set this flag, so we continue reading from the socket, as we're going to bypass HTTP parsing
	session->Reserved_Flags = (unsigned int)ILibTransports_WebSocket;
	session->closePtr = (ILibTransport_ClosePtr)&ILibWebServer_WebSocket_Close;
	session->sendPtr = (ILibTransport_SendPtr)&ILibWebServer_WebSocket_TransportSend;
	session->pendingPtr = (ILibTransport_PendingBytesToSendPtr)&ILibWebServer_Session_GetPendingBytesToSend;

	ILibRemoteLogging_printf(ILibChainGetLogger(session->Reserved_Chain), ILibRemoteLogging_Modules_Microstack_Web, ILibRemoteLogging_Flags_VerbosityLevel_1, "...WebSession[%p] Upgraded to WebSocket", (void*)session);

	return 0;
}


/*! \fn ILibWebServer_Send(struct ILibWebServer_Session *session, struct packetheader *packet)
\brief Send a response on a Session
\param session The ILibWebServer_Session to send the response on
\param packet The packet to respond with
\returns Flag indicating send status.
*/
enum ILibWebServer_Status ILibWebServer_Send(struct ILibWebServer_Session *session, struct packetheader *packet)
{
	char *buffer;
	int bufferSize;
	enum ILibWebServer_Status RetVal = ILibWebServer_ALL_DATA_SENT;

	if (session == NULL || session->SessionInterrupted != 0 || session->Reserved_WebSocket_Request != NULL)
	{
		ILibDestructPacket(packet);
		return(ILibWebServer_INVALID_SESSION);
	}
	session->Reserved4 = 1;
	bufferSize = ILibGetRawPacket(packet, &buffer);

	if ((RetVal = ILibAsyncServerSocket_Send(session->Reserved1, session->Reserved2, buffer, bufferSize, ILibAsyncSocket_MemoryOwnership_CHAIN)) == 0)
	{
		// Completed Send
		if (ILibWebServer_RequestAnswered(session) == ILibWebServer_SEND_RESULTED_IN_DISCONNECT) { RetVal = ILibWebServer_SEND_RESULTED_IN_DISCONNECT; }
	}
	ILibDestructPacket(packet);
	return (RetVal);
}

int ILibWebServer_WebSocket_CreateHeader(char* header, unsigned short FLAGS, unsigned short OPCODE, int payloadLength)
{
	unsigned short header1 = 0;
	int retVal;

	header1 = OPCODE << 8;
	header1 |= FLAGS;

	if (payloadLength < 126)
	{
		retVal = 2;
		header1 |= payloadLength;
		((unsigned short*)header)[0] = htons(header1);
	}
	else if (payloadLength <= 0xFFFF)
	{
		retVal = 4;
		header1 |= 126;
		((unsigned short*)header)[0] = htons(header1);
		((unsigned short*)header)[1] = htons((unsigned short)payloadLength);
	}
	else
	{
		retVal = 10;
		header1 |= 127;
		((unsigned short*)header)[0] = htons(header1);
		((unsigned long long*)(header + 2))[0] = htonl((int)payloadLength);
	}

	return(retVal);
}

enum ILibWebServer_Status ILibWebServer_WebSocket_Send(struct ILibWebServer_Session *session, char* buffer, int bufferLen, ILibWebServer_WebSocket_DataTypes bufferType, enum ILibAsyncSocket_MemoryOwnership userFree, ILibWebServer_WebSocket_FragmentFlags fragmentStatus)
{
	char header[4];
	int headerLen;
	enum ILibWebServer_Status RetVal = ILibWebServer_INVALID_SESSION;

	if (bufferLen > WEBSOCKET_MAX_OUTPUT_FRAMESIZE)
	{
		int i = 0;
		RetVal = ILibWebServer_ALL_DATA_SENT;

		while (i < bufferLen)
		{
			RetVal = ILibWebServer_WebSocket_Send(session, buffer + i, bufferLen - i > WEBSOCKET_MAX_OUTPUT_FRAMESIZE ? WEBSOCKET_MAX_OUTPUT_FRAMESIZE : bufferLen - i, bufferType, ILibAsyncSocket_MemoryOwnership_USER, (i + (bufferLen - i > WEBSOCKET_MAX_OUTPUT_FRAMESIZE ? WEBSOCKET_MAX_OUTPUT_FRAMESIZE : bufferLen - i) < bufferLen) ? ILibWebServer_WebSocket_FragmentFlag_Incomplete : fragmentStatus);
			i += (bufferLen - i > WEBSOCKET_MAX_OUTPUT_FRAMESIZE ? WEBSOCKET_MAX_OUTPUT_FRAMESIZE : bufferLen - i);
		}

		if (userFree == ILibAsyncSocket_MemoryOwnership_CHAIN) { free(buffer); }
		return(RetVal);
	}

	if (fragmentStatus == ILibWebServer_WebSocket_FragmentFlag_Complete)
	{
		if (session->Reserved15 == 0)
		{
			// This is a self contained fragment
			headerLen = ILibWebServer_WebSocket_CreateHeader(header, WEBSOCKET_FIN, (unsigned short)bufferType, bufferLen);
		}
		else
		{
			// Termination of an ongoing Fragment
			session->Reserved15 = 0;
			headerLen = ILibWebServer_WebSocket_CreateHeader(header, WEBSOCKET_FIN, WEBSOCKET_OPCODE_FRAMECONT, bufferLen);
		}
	}
	else
	{
		if (session->Reserved15 == 0)
		{
			// Start a new fragment
			session->Reserved15 = 1;
			headerLen = ILibWebServer_WebSocket_CreateHeader(header, 0, (unsigned short)bufferType, bufferLen);
		}
		else
		{
			// Continuation of an ongoing fragment
			headerLen = ILibWebServer_WebSocket_CreateHeader(header, 0, WEBSOCKET_OPCODE_FRAMECONT, bufferLen);
		}
	}

	sem_wait(&(session->Reserved11)); // We need to do this, because we need to be able to correctly interleave sends
	RetVal = (enum ILibWebServer_Status)ILibAsyncServerSocket_Send(session->Reserved1, session->Reserved2, header, headerLen, ILibAsyncSocket_MemoryOwnership_USER);
	if (bufferLen > 0)
	{
		RetVal = (enum ILibWebServer_Status)ILibAsyncServerSocket_Send(session->Reserved1, session->Reserved2, buffer, bufferLen, userFree);
	}
	sem_post(&(session->Reserved11));
	return(RetVal);
}

/*! \fn ILibWebServer_Send_Raw(struct ILibWebServer_Session *session, char *buffer, int bufferSize, int userFree, int done)
\brief Send a response on a Session, directly specifying the buffers to send
\param session The ILibWebServer_Session to send the response on
\param buffer The buffer to send
\param bufferSize The length of the buffer
\param userFree The ownership flag of the buffer
\param done Flag indicating if this is everything
\returns Send Status
*/
enum ILibWebServer_Status ILibWebServer_Send_Raw(struct ILibWebServer_Session *session, char *buffer, int bufferSize, enum ILibAsyncSocket_MemoryOwnership userFree, ILibWebServer_DoneFlag done)
{
	enum ILibWebServer_Status RetVal = ILibWebServer_ALL_DATA_SENT;

	if (session == NULL || (session != NULL && session->SessionInterrupted != 0))
	{
		if (userFree == ILibAsyncSocket_MemoryOwnership_CHAIN) { free(buffer); }
		return(ILibWebServer_INVALID_SESSION);
	}

	session->Reserved4 = done;
	RetVal = (enum ILibWebServer_Status)ILibAsyncServerSocket_Send(session->Reserved1, session->Reserved2, buffer, bufferSize, userFree);
	if (RetVal == ILibWebServer_ALL_DATA_SENT && done != 0)
	{
		// Completed Send
		RetVal = ILibWebServer_RequestAnswered(session);
	}

	return (RetVal);
}

/*! \fn ILibWebServer_StreamHeader_Raw(struct ILibWebServer_Session *session, int StatusCode,char *StatusData,char *ResponseHeaders, int ResponseHeaders_FREE)
\brief Streams the HTTP header response on a session, directly specifying the buffer
\par
\b DO \b NOT specify Content-Length or Transfer-Encoding.
\param session The ILibWebServer_Session to send the response on
\param StatusCode The HTTP status code, eg: \b 200
\param StatusData The HTTP status data, eg: \b OK
\param ResponseHeaders Additional HTTP header fields
\param ResponseHeaders_FREE Ownership flag of the addition http header fields
\returns Send Status
*/
enum ILibWebServer_Status ILibWebServer_StreamHeader_Raw(struct ILibWebServer_Session *session, int StatusCode, char *StatusData, char *ResponseHeaders, enum ILibAsyncSocket_MemoryOwnership ResponseHeaders_FREE)
{
	int len;
	char *temp;
	int tempLength;
	char *buffer;
	int bufferLength;
	struct packetheader *hdr;
	struct parser_result *pr, *pr2;
	struct parser_result_field *prf;
	enum ILibWebServer_Status RetVal = ILibWebServer_ALL_DATA_SENT;

	if (session == NULL || session->SessionInterrupted != 0 || session->Reserved_WebSocket_Request != NULL)
	{
		if (ResponseHeaders_FREE == ILibAsyncSocket_MemoryOwnership_CHAIN) { free(ResponseHeaders); }
		return ILibWebServer_INVALID_SESSION;
	}

	hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	if (hdr == NULL)
	{
		if (ResponseHeaders_FREE == ILibAsyncSocket_MemoryOwnership_CHAIN) { free(ResponseHeaders); }
		return ILibWebServer_INVALID_SESSION;
	}

	//
	// Allocate the response header buffer
	// ToDo: May want to make the response version dynamic or at least #define
	//
	if ((buffer = (char*)malloc(20 + strlen(StatusData))) == NULL) ILIBCRITICALEXIT(254);
	if (buffer == NULL) ILIBCRITICALEXIT(254);
	bufferLength = snprintf(buffer, 20 + strlen(StatusData), "HTTP/%s %d %s", HTTPVERSION, StatusCode, StatusData);

	//
	// Send the first portion of the headers across
	//
	RetVal = ILibWebServer_Send_Raw(session, buffer, bufferLength, ILibAsyncSocket_MemoryOwnership_CHAIN, ILibWebServer_DoneFlag_NotDone);
	if (RetVal != ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET && RetVal != ILibWebServer_SEND_RESULTED_IN_DISCONNECT)
	{
		//
		// The Send went through
		//
		//{{{ REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT--> }}}
		if (!(hdr->VersionLength == 3 && memcmp(hdr->Version, "1.0", 3) == 0))
		{
			//
			// If this was not an HTTP/1.0 response, then we need to chunk
			//
			RetVal = ILibWebServer_Send_Raw(session, "\r\nTransfer-Encoding: chunked", 28, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_NotDone);
		}
		else
		{
			//{{{ <--REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT }}}
			//
			// Since we are streaming over HTTP/1.0 , we are required to close the socket when done
			//
			session->Reserved6 = 1;
			//{{{ REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT--> }}}
		}
		//{{{ <--REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT }}}
		if (ResponseHeaders != NULL && RetVal != ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET && RetVal != ILibWebServer_SEND_RESULTED_IN_DISCONNECT)
		{
			//
			// Send the user specified headers
			//
			len = (int)strlen(ResponseHeaders);
			if (len < MAX_HEADER_LENGTH)
			{
				RetVal = ILibWebServer_Send_Raw(session, ResponseHeaders, len, ResponseHeaders_FREE, ILibWebServer_DoneFlag_NotDone);
			}
			else
			{
				pr = ILibParseString(ResponseHeaders, 0, len, "\r\n", 2);
				prf = pr->FirstResult;
				while (prf != NULL)
				{
					if (prf->datalength != 0)
					{
						pr2 = ILibParseString(prf->data, 0, prf->datalength, ":", 1);
						if (pr2->NumResults != 1)
						{
							RetVal = ILibWebServer_Send_Raw(session, "\r\n", 2, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_NotDone);
							if (RetVal != ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET && RetVal != ILibWebServer_SEND_RESULTED_IN_DISCONNECT)
							{
								RetVal = ILibWebServer_Send_Raw(session, pr2->FirstResult->data, pr2->FirstResult->datalength + 1, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_NotDone);
								if (RetVal != ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET && RetVal != ILibWebServer_SEND_RESULTED_IN_DISCONNECT)
								{
									tempLength = ILibFragmentText(prf->data + pr2->FirstResult->datalength + 1, prf->datalength - pr2->FirstResult->datalength - 1, "\r\n ", 3, MAX_HEADER_LENGTH, &temp);
									RetVal = ILibWebServer_Send_Raw(session, temp, tempLength, ILibAsyncSocket_MemoryOwnership_CHAIN, ILibWebServer_DoneFlag_NotDone);
								}
								else
								{
									ILibDestructParserResults(pr2);
									break;
								}
							}
							else
							{
								ILibDestructParserResults(pr2);
								break;
							}
						}
						ILibDestructParserResults(pr2);
					}
					prf = prf->NextResult;
				}
				ILibDestructParserResults(pr);
			}
		}
		if (RetVal != ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET && RetVal != ILibWebServer_SEND_RESULTED_IN_DISCONNECT)
		{
			//
			// Send the Header Terminator
			//
			return(ILibWebServer_Send_Raw(session, "\r\n\r\n", 4, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_NotDone));
		}
		else
		{
			if (RetVal != ILibWebServer_ALL_DATA_SENT && session->Reserved10 != NULL)
			{
				session->Reserved10 = NULL;
			}
			return (RetVal);
		}
	}
	//
	// ToDo: May want to check logic if the sends didn't go through
	//
	if (RetVal != ILibWebServer_ALL_DATA_SENT && session->Reserved10 != NULL)
	{
		session->Reserved10 = NULL;
	}
	return (RetVal);
}

/*! \fn ILibWebServer_StreamHeader(struct ILibWebServer_Session *session, struct packetheader *header)
\brief Streams the HTTP header response on a session
\par
\b DO \b NOT specify Transfer-Encoding.
\param session The ILibWebServer_Session to send the response on
\param header The headers to return
\returns Send Status
*/
enum ILibWebServer_Status ILibWebServer_StreamHeader(struct ILibWebServer_Session *session, struct packetheader *header)
{
	struct packetheader *hdr;
	int bufferLength;
	char *buffer;
	enum ILibWebServer_Status RetVal = ILibWebServer_INVALID_SESSION;

	if (session == NULL || session->SessionInterrupted != 0 || session->Reserved_WebSocket_Request != NULL)
	{
		ILibDestructPacket(header);
		return(ILibWebServer_INVALID_SESSION);
	}

	hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	if (hdr == NULL)
	{
		ILibDestructPacket(header);
		return(ILibWebServer_INVALID_SESSION);
	}

	//{{{ REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT--> }}}
	if (!(hdr->VersionLength == 3 && memcmp(hdr->Version, "1.0", 3) == 0))
	{
		//
		// If this isn't an HTTP/1.0 connection, remove content-length, and chunk the response
		//
		ILibDeleteHeaderLine(header, "Content-Length", 14);
		//
		// Add the Transfer-Encoding header
		//
		ILibAddHeaderLine(header, "Transfer-Encoding", 17, "chunked", 7);
	}
	else
	{
		//{{{ <--REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT }}}
		// Check to see if they gave us a Content-Length
		if (ILibGetHeaderLine(hdr, "Content-Length", 14) == NULL)
		{
			//
			// If it wasn't, we'll set the CloseOverrideFlag, because in order to be compliant
			// we must close the socket when done
			//
			session->Reserved6 = 1;
		}
		//{{{ REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT--> }}}
	}
	//{{{ <--REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT }}}
	//
	// Grab the bytes and send it
	//
	bufferLength = ILibGetRawPacket(header, &buffer);
	//
	// Since ILibGetRawPacket allocates memory, we give ownership to the MicroStack, and
	// let it take care of it
	//
	RetVal = ILibWebServer_Send_Raw(session, buffer, bufferLength, ILibAsyncSocket_MemoryOwnership_CHAIN, ILibWebServer_DoneFlag_NotDone);
	ILibDestructPacket(header);
	if (RetVal != ILibWebServer_ALL_DATA_SENT && session->Reserved10 != NULL)
	{
		session->Reserved10 = NULL;
	}
	return (RetVal);
}

/*! \fn ILibWebServer_StreamBody(struct ILibWebServer_Session *session, char *buffer, int bufferSize, int userFree, int done)
\brief Streams the HTTP body on a session
\param session The ILibWebServer_Session to send the response on
\param buffer The buffer to send
\param bufferSize The size of the buffer
\param userFree The ownership flag of the buffer
\param done Flag indicating if this is everything
\returns Send Status
*/
enum ILibWebServer_Status ILibWebServer_StreamBody(struct ILibWebServer_Session *session, char *buffer, int bufferSize, enum ILibAsyncSocket_MemoryOwnership userFree, ILibWebServer_DoneFlag done)
{
	struct packetheader *hdr;
	char *hex;
	int hexLen;
	enum ILibWebServer_Status RetVal = ILibWebServer_INVALID_SESSION;

	if (session == NULL || session->SessionInterrupted != 0 || session->Reserved_WebSocket_Request != NULL)
	{
		if (userFree == ILibAsyncSocket_MemoryOwnership_CHAIN) { free(buffer); }
		return(ILibWebServer_INVALID_SESSION);
	}

	hdr = ILibWebClient_GetHeaderFromDataObject(session->Reserved3);
	if (hdr == NULL)
	{
		if (userFree == ILibAsyncSocket_MemoryOwnership_CHAIN) { free(buffer); }
		return(ILibWebServer_INVALID_SESSION);
	}

	//{{{ REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT--> }}}
	if (hdr->VersionLength == 3 && memcmp(hdr->Version, "1.0", 3) == 0)
	{
		//{{{ <--REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT }}}
		//
		// This is HTTP/1.0 , so we don't need to do anything special
		//
		if (bufferSize > 0)
		{
			//
			// If there is actually something to send, then send it
			//
			RetVal = ILibWebServer_Send_Raw(session, buffer, bufferSize, userFree, done);
		}
		else if (done != 0)
		{
			//
			// Nothing to send?
			//
			RetVal = ILibWebServer_RequestAnswered(session);
		}
		//{{{ REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT--> }}}
	}
	else
	{
		//
		// This is HTTP/1.1+ , so we need to chunk the body
		//
		if (bufferSize > 0)
		{
			//
			// Calculate the length of the body in hex, and create the chunk header
			//
			if ((hex = (char*)malloc(16)) == NULL) ILIBCRITICALEXIT(254);
			hexLen = snprintf(hex, 16, "%X\r\n", bufferSize);
			RetVal = ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET;

			//
			// Send the chunk header
			//
			if (ILibWebServer_Send_Raw(session, hex, hexLen, ILibAsyncSocket_MemoryOwnership_CHAIN, ILibWebServer_DoneFlag_NotDone) != ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET)
			{
				//
				// Send the data
				//
				if (ILibWebServer_Send_Raw(session, buffer, bufferSize, userFree, ILibWebServer_DoneFlag_NotDone) != ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET)
				{
					//
					// The data must be terminated with a CRLF, (don't ask why, it just does)
					//
					RetVal = ILibWebServer_Send_Raw(session, "\r\n", 2, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_NotDone);
				}
				else
				{
					RetVal = ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET;
				}
			}
			else
			{
				RetVal = ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET;
				//
				// We didn't send the buffer yet, so we need to free it if
				// we own the memory
				//
				if (userFree == ILibAsyncSocket_MemoryOwnership_CHAIN)
				{
					free(buffer);
				}
			}
			//
			// These protections with the ILibAsyncSocket_SEND_ON_CLOSED_SOCKET_ERROR check is
			// to prevent broken pipe errors
			//

		}
		if (done == ILibWebServer_DoneFlag_Done && RetVal != ILibWebServer_TRIED_TO_SEND_ON_CLOSED_SOCKET && RetVal != ILibWebServer_SEND_RESULTED_IN_DISCONNECT &&
			!(hdr->DirectiveLength == 4 && strncasecmp(hdr->Directive, "HEAD", 4) == 0))
		{
			//
			// Terminate the chunk
			//
			RetVal = ILibWebServer_Send_Raw(session, "0\r\n\r\n", 5, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_Done);
		}
		else if (done == ILibWebServer_DoneFlag_Done && RetVal >= 0)
		{
			RetVal = ILibWebServer_RequestAnswered(session);
		}
	}
	//{{{ <--REMOVE_THIS_FOR_HTTP/1.0_ONLY_SUPPORT }}}

	if (RetVal != ILibWebServer_ALL_DATA_SENT && session->Reserved10 != NULL)
	{
		session->Reserved10 = NULL;
	}
	return(RetVal);
}


/*! \fn ILibWebServer_GetRemoteInterface(struct ILibWebServer_Session *session)
\brief Returns the remote interface of an HTTP session
\param session The ILibWebServer_Session to query
\returns The remote interface
*/
int ILibWebServer_GetRemoteInterface(struct ILibWebServer_Session *session, struct sockaddr *remoteAddress)
{
	return ILibAsyncSocket_GetRemoteInterface(session->Reserved2, remoteAddress);
}

/*! \fn ILibWebServer_GetLocalInterface(struct ILibWebServer_Session *session)
\brief Returns the local interface of an HTTP session
\param session The ILibWebServer_Session to query
\returns The local interface
*/
int ILibWebServer_GetLocalInterface(struct ILibWebServer_Session *session, struct sockaddr *localAddress)
{
	return ILibAsyncSocket_GetLocalInterface(session->Reserved2, localAddress);
}



/*! \fn ILibWebServer_RegisterVirtualDirectory(ILibWebServer_ServerToken WebServerToken, char *vd, int vdLength, ILibWebServer_VirtualDirectory OnVirtualDirectory, void *user)
\brief Registers a Virtual Directory with the ILibWebServer
\param WebServerToken The ILibWebServer to register with
\param vd The virtual directory path
\param vdLength The length of the path
\param OnVirtualDirectory The Virtual Directory handler
\param user User state info to pass on
\returns 0 if successful, nonzero otherwise
*/
int ILibWebServer_RegisterVirtualDirectory(ILibWebServer_ServerToken WebServerToken, char *vd, int vdLength, ILibWebServer_VirtualDirectory OnVirtualDirectory, void *user)
{
	struct ILibWebServer_VirDir_Data *data;
	struct ILibWebServer_StateModule *s = (struct ILibWebServer_StateModule*)WebServerToken;
	if (s->VirtualDirectoryTable == NULL)
	{
		// If no Virtual Directories have been registered yet, we need to initialize the lookup table
		s->VirtualDirectoryTable = ILibInitHashTree();
	}

	if (ILibHasEntry(s->VirtualDirectoryTable, vd, vdLength) != 0)
	{
		// This Virtual Directory was already registered
		return 1;
	}
	else
	{
		// Add the necesary info into the lookup table
		if ((data = (struct ILibWebServer_VirDir_Data*)malloc(sizeof(struct ILibWebServer_VirDir_Data))) == NULL) ILIBCRITICALEXIT(254);
		data->callback = (voidfp)OnVirtualDirectory;
		data->user = user;
		ILibAddEntry(s->VirtualDirectoryTable, vd, vdLength, data);
	}
	return 0;
}

/*! \fn ILibWebServer_UnRegisterVirtualDirectory(ILibWebServer_ServerToken WebServerToken, char *vd, int vdLength)
\brief UnRegisters a Virtual Directory from the ILibWebServer
\param WebServerToken The ILibWebServer to unregister from
\param vd The virtual directory path
\param vdLength The length of the path
\returns 0 if successful, nonzero otherwise
*/
int ILibWebServer_UnRegisterVirtualDirectory(ILibWebServer_ServerToken WebServerToken, char *vd, int vdLength)
{
	struct ILibWebServer_StateModule *s = (struct ILibWebServer_StateModule*)WebServerToken;
	if (ILibHasEntry(s->VirtualDirectoryTable, vd, vdLength) != 0)
	{
		// The virtual directory registry was found, delete it
		free(ILibGetEntry(s->VirtualDirectoryTable, vd, vdLength));
		ILibDeleteEntry(s->VirtualDirectoryTable, vd, vdLength);
		return 0;
	}
	else
	{
		// Couldn't find the virtual directory registry
		return 1;
	}
}

/*! \fn ILibWebServer_AddRef(struct ILibWebServer_Session *session)
\brief Reference Counter for an \a ILibWebServer_Session object
\param session The ILibWebServer_Session object
*/
void ILibWebServer_AddRef(struct ILibWebServer_Session *session)
{
	SESSION_TRACK(session, "AddRef");
	sem_wait(&(session->Reserved11));
	++session->Reserved12;
	sem_post(&(session->Reserved11));
}

/*! \fn ILibWebServer_Release(struct ILibWebServer_Session *session)
\brief Decrements reference counter for \a ILibWebServer_Session object
\par
When the counter reaches 0, the object is freed
\param session The ILibWebServer_Session object
*/
void ILibWebServer_Release(struct ILibWebServer_Session *session)
{
	int OkToFree = 0;

	SESSION_TRACK(session, "Release");
	sem_wait(&(session->Reserved11));
	if (--session->Reserved12 == 0)
	{
		//
		// There are no more outstanding references, so we can
		// free this thing
		//
		OkToFree = 1;
	}
	sem_post(&(session->Reserved11));
	if (session->SessionInterrupted == 0)
	{
		ILibLifeTime_Remove(((struct ILibWebServer_StateModule*)session->Parent)->LifeTime, session);
	}

	if (OkToFree)
	{
		if (session->SessionInterrupted == 0)
		{
			ILibWebClient_DestroyWebClientDataObject(session->Reserved3);
		}
		SESSION_TRACK(session, "** Destroyed **");
		sem_destroy(&(session->Reserved11));
		if (session->Reserved_DigestTable != NULL) { ILibDestroyHashTree(session->Reserved_DigestTable); }
		if (session->Reserved17 != NULL)	{ free(session->Reserved17); }
		if (session->Reserved_WebSocket_Request != NULL) { ILibDestructPacket((struct packetheader*)session->Reserved_WebSocket_Request); }
		free(session);
	}
}
/*! \fn void ILibWebServer_DisconnectSession(struct ILibWebServer_Session *session)
\brief Terminates an ILibWebServer_Session.
\par
<B>Note:</B> Normally this should never be called, as the session is automatically
managed by the system, such that it can take advantage of persistent connections and such.
\param session The ILibWebServer_Session object to disconnect
*/
void ILibWebServer_DisconnectSession(struct ILibWebServer_Session *session)
{
	ILibWebClient_Disconnect(session->Reserved3);
}
/*! \fn void ILibWebServer_Pause(struct ILibWebServer_Session *session)
\brief Pauses the ILibWebServer_Session, such that reading of more data off the network is
temporarily suspended.
\par
<B>Note:</B> This method <B>MUST</B> only be called from either the \a ILibWebServer_Session_OnReceive or \a ILibWebServer_VirtualDirectory handlers.
\param session The ILibWebServer_Session object to pause.
*/
__inline void ILibWebServer_Pause(struct ILibWebServer_Session *session)
{
	ILibWebClient_Pause(session->Reserved3);
}
/*! \fn void ILibWebServer_Resume(struct ILibWebServer_Session *session)
\brief Resumes a paused ILibWebServer_Session object.
\param session The ILibWebServer_Session object to resume.
*/
__inline void ILibWebServer_Resume(struct ILibWebServer_Session *session)
{
	ILibWebClient_Resume(session->Reserved3);
}
/*! \fn void ILibWebServer_OverrideReceiveHandler(struct ILibWebServer_Session *session, ILibWebServer_Session_OnReceive OnReceive)
\brief Overrides the Receive handler, so that the passed in handler will get called whenever data is received.
\param session The ILibWebServer_Session to hijack.
\param OnReceive The handler to handle the received data
*/
__inline void ILibWebServer_OverrideReceiveHandler(struct ILibWebServer_Session *session, ILibWebServer_Session_OnReceive OnReceive)
{
	session->OnReceive = OnReceive;
	session->Reserved13 = 1;
}

/*
void *ILibWebServer_FakeConnect(void *module, void *cd)
{
struct ILibWebServer_StateModule *wsm = (struct ILibWebServer_StateModule*)module;
return ILibAsyncServerSocket_FakeConnect(wsm->ServerSocket, cd);
}
*/

// Private method used to send a file to the web session asynchronously
void ILibWebServer_StreamFileSendOK(struct ILibWebServer_Session *sender)
{
	FILE* pfile;
	size_t len;
	int status = 0;

	pfile = (FILE*)sender->User3;
	while ((len = fread(ILibScratchPad, 1, sizeof(ILibScratchPad), pfile)) != 0)
	{
		status = ILibWebServer_StreamBody(sender, ILibScratchPad, (int)len, ILibAsyncSocket_MemoryOwnership_USER, ILibWebServer_DoneFlag_NotDone);
		if (status != ILibWebServer_ALL_DATA_SENT || len < sizeof(ILibScratchPad)) break;
	}

	if (len < sizeof(ILibScratchPad) || status < 0)
	{
		// Finished sending the file or got a send error, close the session
		ILibWebServer_StreamBody(sender, NULL, 0, ILibAsyncSocket_MemoryOwnership_STATIC, ILibWebServer_DoneFlag_Done);
		fclose(pfile);
	}
}

// Streams a file to the web session asynchronously, closes the session when done.
// Caller must supply a valid file handle.
void ILibWebServer_StreamFile(struct ILibWebServer_Session *session, FILE* pfile)
{
	session->OnSendOK = ILibWebServer_StreamFileSendOK;
	session->User3 = (void*)pfile;
	ILibWebServer_StreamFileSendOK(session);
}

