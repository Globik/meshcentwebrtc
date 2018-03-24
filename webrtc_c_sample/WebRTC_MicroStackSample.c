/*
Copyright 2015 Intel Corporation

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

//
// WebRTC MicroStackSample.cpp : Defines the entry point for the console application.
// #define _ICE_DEBUG_TRANSACTION_ // enabled WebRTC debug messages
//

#if defined(WIN32) && !defined(_WIN32_WCE)
#include "stdafx.h"
#include <WinSock2.h>
#include <WS2tcpip.h>
#define _CRTDBG_MAP_ALLOC
#include <crtdbg.h>
#endif
#define green "\x1b[32m"
#define yellow "\x1b[33m"
#define red "\x1b[31m"
#define rst "\x1b[0m"
#include "Microstack/ILibParsers.h"
#include "Microstack/ILibAsyncSocket.h"
#include "Microstack/ILibWebRTC.h"
#include "core/utils.h"
#include "Microstack/ILibWrapperWebRTC.h"
#include "SimpleRendezvousServer.h"

#if defined(WIN32) && !defined(snprintf) && _MSC_VER < 1900
#define snprintf(dst, len, frm, ...) _snprintf_s(dst, len, _TRUNCATE, frm, __VA_ARGS__)
#endif

char *htmlBody, *passiveHtmlBody, *wshtmlbody;
int htmlBodyLength, passiveHtmlBodyLength, wshtmlBodyLength;

ILibWrapper_WebRTC_ConnectionFactory mConnectionFactory;
ILibWrapper_WebRTC_Connection mConnection;
ILibWrapper_WebRTC_DataChannel *mDataChannel = NULL;
SimpleRendezvousServer mServer;

void* chain;
char *stunServerList[] = { "stun.ekiga.net", "stun.ideasip.com", "stun.schlund.de", "stunserver.org", "stun.softjoys.com", "stun.voiparound.com", "stun.voipbuster.com", "stun.voipstunt.com", "stun.voxgratia.org" };
int useStun = 0;





// This is called when Data is received on the WebRTC Data Channel
void OnDataChannelData(ILibWrapper_WebRTC_DataChannel *dataChannel, char* buffer, int bufferLen)
{
	buffer[bufferLen] = 0;
	printf(red"Received data on [%s]: %s\r\n"rst, dataChannel->channelName, buffer);
}

// This is called when the Data Channel was closed
void OnDataChannelClosed(ILibWrapper_WebRTC_DataChannel *dataChannel)
{
	printf(red"DataChannel [%s]:%u was closed\r\n"rst, dataChannel->channelName, dataChannel->streamId);
}

// This is called when the remote ACK's our DataChannel creation request
void OnDataChannelAck(ILibWrapper_WebRTC_DataChannel *dataChannel)
{
	mDataChannel = dataChannel;
	printf(red"DataChannel [%s] was successfully ACK'ed\r\n"rst, dataChannel->channelName);
	mDataChannel->OnStringData = (ILibWrapper_WebRTC_DataChannel_OnData)&OnDataChannelData;
	mDataChannel->OnClosed = (ILibWrapper_WebRTC_DataChannel_OnClosed)&OnDataChannelClosed;
}

// This is called when a WebRTC Connection is established or disconnected
void WebRTCConnectionSink(ILibWrapper_WebRTC_Connection connection, int connected)
{
	printf(red"webrtcconnectionsink\n"rst);
	if(connected)
	{
		printf(red"WebRTC connection Established. [%s]\r\n"rst, ILibWrapper_WebRTC_Connection_DoesPeerSupportUnreliableMode(connection)==0?"RELIABLE Only":"UNRELIABLE Supported");
		ILibWrapper_WebRTC_DataChannel_Create(connection, "MyDataChannel", 13, &OnDataChannelAck);
	}
	else
	{
		printf(red"WebRTC connection is closed.\r\n"rst);
		mConnection = NULL;
		mDataChannel = NULL;
	}
}

// This is called when the remote side created a data channel
void WebRTCDataChannelSink(ILibWrapper_WebRTC_Connection connection, ILibWrapper_WebRTC_DataChannel *dataChannel)
{
	printf(red"WebRTC Data Channel (%u:%s) was created.\r\n"rst, dataChannel->streamId, dataChannel->channelName);
	mDataChannel = dataChannel;
	mDataChannel->OnStringData = (ILibWrapper_WebRTC_DataChannel_OnData)&OnDataChannelData;
	mDataChannel->OnClosed = (ILibWrapper_WebRTC_DataChannel_OnClosed)&OnDataChannelClosed;
}

void WebRTCConnectionSendOkSink(ILibWrapper_WebRTC_Connection connection)
{
	printf(red"webrtcconnectionsendoksink\n"rst);
	UNREFERENCED_PARAMETER(connection);
}

// If we launched this sample with "STUN", then this is called when a STUN candidate is found while setting an offer
void CandidateSink(ILibWrapper_WebRTC_Connection connection, struct sockaddr_in6* candidate)
{
	printf(red"candidatesink!\n"rst);
	SimpleRendezvousServer sender;
	SimpleRendezvousServerToken token;
	char *sdp = ILibWrapper_WebRTC_Connection_AddServerReflexiveCandidateToLocalSDP(connection, candidate);
	
	ILibWrapper_WebRTC_Connection_GetUserData(connection, &sender, &token, NULL);
	if(SimpleRendezvousServer_WebSocket_IsWebSocket(token)==0)
	{
		printf(red"ws is ws(token?)\n"rst);
		SimpleRendezvousServer_Respond(sender, token, 1, sdp, strlen(sdp), ILibAsyncSocket_MemoryOwnership_CHAIN); // Send the SDP to the remote side
	}
	else
	{
		printf(red"not token?\n"rst);
		SimpleRendezvousServer_WebSocket_Send(token, SimpleRendezvousServer_WebSocket_DataType_TEXT, sdp, strlen(sdp), ILibAsyncSocket_MemoryOwnership_CHAIN, SimpleRendezvousServer_FragmentFlag_Complete);
	}
}

// If we launched this sample with "STUN", then this is called when a STUN candidate is found while generating an offer

void PassiveCandidateSink(ILibWrapper_WebRTC_Connection connection, struct sockaddr_in6* candidate)
{
	printf("While generating an offer?\n"rst);
	SimpleRendezvousServer sender;
	SimpleRendezvousServerToken token;
	char *offer;
	char *encodedOffer;
	int encodedOfferLen;
	char *h1;

	ILibWrapper_WebRTC_Connection_GetUserData(connection, &sender, &token, (void**)&h1);

	offer = ILibWrapper_WebRTC_Connection_AddServerReflexiveCandidateToLocalSDP(connection, candidate);
	printf(green"ofer: \n %s\n"rst,offer);
	encodedOfferLen = ILibBase64Encode((unsigned char*)offer, strlen(offer),(unsigned char**)&encodedOffer);

	h1 = ILibString_Replace(passiveHtmlBody, passiveHtmlBodyLength, "/*{{{SDP}}}*/", 13, encodedOffer, encodedOfferLen);

	free(offer);
	free(encodedOffer);
	SimpleRendezvousServer_Respond(sender, token, 1, h1, strlen(h1), ILibAsyncSocket_MemoryOwnership_CHAIN); // Send the SDP to the remote side
}

void OnWebSocket(SimpleRendezvousServerToken sender, int InterruptFlag, struct packetheader *header, char *bodyBuffer,
				 int bodyBufferLen, SimpleRendezvousServer_WebSocket_DataTypes bodyBufferType, SimpleRendezvousServer_DoneFlag done)
{	
	printf(red"on websocket\n"rst);
	
	if(done == SimpleRendezvousServer_DoneFlag_NotDone)
	{
		printf(yellow"We have the entire offer\n"rst);
			char *offer;
		if (mConnection == NULL)
		{
			printf(yellow"The browser initiated the SDP offer, so we have to create a connection and set the offer\n"rst);
			mConnection = ILibWrapper_WebRTC_ConnectionFactory_CreateConnection(mConnectionFactory, 
																				&WebRTCConnectionSink, 
																				&WebRTCDataChannelSink, 
																				&WebRTCConnectionSendOkSink);
			ILibWrapper_WebRTC_Connection_SetStunServers(mConnection, stunServerList, 9);

			if (useStun==0)
			{
				printf(red"stun is 0\n"rst);
				offer = ILibWrapper_WebRTC_Connection_SetOffer(mConnection, bodyBuffer, bodyBufferLen, NULL);
				SimpleRendezvousServer_WebSocket_Send(sender, SimpleRendezvousServer_WebSocket_DataType_TEXT, offer, strlen(offer), ILibAsyncSocket_MemoryOwnership_CHAIN, SimpleRendezvousServer_FragmentFlag_Complete);
			}
			else
			{
				printf(yellow"We're freeing this, becuase we'll generate the offer in the candidate callback...\n"rst);
				// The best way, is to return this offer, and update the candidate incrementally, but that is for another sample
				ILibWrapper_WebRTC_Connection_SetUserData(mConnection, NULL, sender, NULL);
				free(ILibWrapper_WebRTC_Connection_SetOffer(mConnection, bodyBuffer, bodyBufferLen, &CandidateSink));
			}
		}
		else
		{
			printf(yellow"We inititiated the SDP exchange, so the browser is just giving us a response... Even tho, this will generate a counter-response\n"rst);
			// we don't need to send it back to the browser, so we'll just drop it.
			printf(red"Setting Offer...\r\n"rst);
			free(ILibWrapper_WebRTC_Connection_SetOffer(mConnection, bodyBuffer, bodyBufferLen, NULL));	
		}
	}
}
void OnWebSocketClosed(SimpleRendezvousServerToken sender)
{
	printf("websock closed\n");
}

// This gets called When the browser hits one of the two URLs
void Rendezvous_OnGet(SimpleRendezvousServer sender, SimpleRendezvousServerToken token, char* path, char* receiver)
{
	printf(green"on Get\n"rst);
	if(strcmp(path, "/active")==0)
	{
		if(mConnection != NULL) { ILibWrapper_WebRTC_Connection_Disconnect(mConnection); mConnection = NULL; }

#ifdef MICROSTACK_TLS_DETECT
		if(SimpleRendezvousServer_IsTLS(token)!=0)
		{
			printf(" Received Client Request: [TLS]\r\n");
		}
		else
		{
			printf(" Received Client Request: [No-TLS]\r\n");
		}
#endif

		SimpleRendezvousServer_Respond(sender, token, 1, htmlBody, htmlBodyLength, ILibAsyncSocket_MemoryOwnership_USER);  // Send the HTML to the Browser
	}
	else if(strcmp(path, "/websocket")==0)
	{
		printf(red"/websocket\n"rst);
		int v = SimpleRendezvousServer_WebSocket_IsRequest(token);

		if(SimpleRendezvousServer_IsAuthenticated(token, "www.meshcentral.com", 19)!=0)
		{
			char* name = SimpleRendezvousServer_GetUsername(token);
			if(SimpleRendezvousServer_ValidatePassword(token, "bryan", 5)!=0)
			{
				printf(red"rendevous respond\n"rst);
				SimpleRendezvousServer_Respond(sender, token, 1, wshtmlbody, wshtmlBodyLength, ILibAsyncSocket_MemoryOwnership_USER);  // Send the HTML to the Browser
			}
		}
	}
	else if(strcmp(path, "/websocketInit")==0)
	{
		printf(green"/websocketInit\n"rst);
		int v = SimpleRendezvousServer_WebSocket_IsRequest(token);
		SimpleRendezvousServer_UpgradeWebSocket(token, 65535, &OnWebSocket, &OnWebSocketClosed);
	}
	else if(strcmp(path, "/passive")==0)
	{
		printf("/passive\n");
		char* offer;
		char* encodedOffer = NULL;
		int encodedOfferLen;
		char *h1;
		
		if(mConnection != NULL){ ILibWrapper_WebRTC_Connection_Disconnect(mConnection); mConnection = NULL; }

		mConnection = ILibWrapper_WebRTC_ConnectionFactory_CreateConnection(mConnectionFactory, &WebRTCConnectionSink, &WebRTCDataChannelSink, &WebRTCConnectionSendOkSink);
		ILibWrapper_WebRTC_Connection_SetStunServers(mConnection, stunServerList, 9);

		if(useStun==0)
		{
			//
			// If STUN was not enabled, then we just generate an offer, and encode it as Base64 into the HTML file to send to the browser
			//
			printf("Generating Offer...\r\n");
			offer = ILibWrapper_WebRTC_Connection_GenerateOffer(mConnection, NULL);
			printf("Encoding Offer...\r\n");
			encodedOfferLen = ILibBase64Encode((unsigned char*)offer, strlen(offer),(unsigned char**)&encodedOffer);
			h1 = ILibString_Replace(passiveHtmlBody, passiveHtmlBodyLength, "/*{{{SDP}}}*/", 13, encodedOffer, encodedOfferLen);

			printf("Sending Offer...\r\n");

			free(offer);
			free(encodedOffer);
			SimpleRendezvousServer_Respond(sender, token, 1, h1, strlen(h1), ILibAsyncSocket_MemoryOwnership_CHAIN); // Send the HTML/OFFER to the browser
		}
		else
		{
			//
			// If STUN was enabled, to simplify this sample app, we'll generate the offer, but we won't do anything with it, as we'll wait until
			printf(red"we get called back saying that candidate gathering was done\n"rst);
			//
			ILibWrapper_WebRTC_Connection_SetUserData(mConnection, sender, token, NULL);
			free(ILibWrapper_WebRTC_Connection_GenerateOffer(mConnection, &PassiveCandidateSink));
			// We're immediately freeing, becuase we're going to re-generate the offer when we get the candidate callback
			// It would be better if we sent this offer, and just passed along the candidates separately, but that is for another sample, since that requires more complex logic.
		}
	}
	else
	{
		printf(red"If we got a GET request for something other than ACTIVE or PASSIVE, we'll just return an error\n"rst);
		SimpleRendezvousServer_Respond(sender, token, 0, NULL, 0, ILibAsyncSocket_MemoryOwnership_CHAIN); 
	}
}

// This will get called by the javascript in the Browser, to pass us offers
void Rendezvous_OnPost(SimpleRendezvousServer sender, SimpleRendezvousServerToken token, char* path, char* data, int dataLen)
{
	printf(red"onpost\n"rst);
	char *offer;
	if (mConnection == NULL)
	{
		 printf("The browser initiated the SDP offer, so we have to create a connection and set the offer\n");
		mConnection = ILibWrapper_WebRTC_ConnectionFactory_CreateConnection(mConnectionFactory, &WebRTCConnectionSink, &WebRTCDataChannelSink, &WebRTCConnectionSendOkSink);
		ILibWrapper_WebRTC_Connection_SetStunServers(mConnection, stunServerList, 9);

		if (useStun==0)
		{
			printf("stun is null in on post\n");
			offer = ILibWrapper_WebRTC_Connection_SetOffer(mConnection, data, dataLen, NULL);
			SimpleRendezvousServer_Respond(sender, token, 1, offer, strlen(offer), ILibAsyncSocket_MemoryOwnership_CHAIN);
		}
		else
		{
			printf(red"We're freeing this, becuase we'll generate the offer in the candidate callback...\n"rst);
			// The best way, is to return this offer, and update the candidate incrementally, but that is for another sample
			ILibWrapper_WebRTC_Connection_SetUserData(mConnection, sender, token, NULL);
			free(ILibWrapper_WebRTC_Connection_SetOffer(mConnection, data, dataLen, &CandidateSink));
		}
	}
	else
	{
		printf("We inititiated the SDP exchange, so the browser is just giving us a response... Even tho, this will generate a counter-response\n");
		// we don't need to send it back to the browser, so we'll just drop it.
		data[dataLen] = 0;
		printf("Setting Offer...\r\n");
		free(ILibWrapper_WebRTC_Connection_SetOffer(mConnection, data, dataLen, NULL));
		SimpleRendezvousServer_Respond(sender, token, 1, NULL, 0, ILibAsyncSocket_MemoryOwnership_STATIC);		
	}
}

#if defined(WIN32)
BOOL CtrlHandler( DWORD fdwCtrlType ) 
{
	switch( fdwCtrlType ) 
	{ 
		// Handle the CTRL-C signal. 
		case CTRL_C_EVENT: 
		case CTRL_BREAK_EVENT: 
			mDataChannel = NULL;
			ILibStopChain(chain); // Shutdown the chain
			return( TRUE );
		default: 
			return FALSE; 
	} 
}
#endif
#if defined(_POSIX)
void BreakSink(int s)
{
	UNREFERENCED_PARAMETER( s );

	signal(SIGINT, SIG_IGN);	// To ignore any more ctrl c interrupts
	
	ILibStopChain(chain); // Shutdown the Chain
}
#endif
void Run()
{
	char temp[1024];
	char* line;

	while(ILibIsChainBeingDestroyed(chain)==0)
	{
		line = fgets(temp, 1024, stdin);

		if (mDataChannel != NULL && line!=NULL)
		{
			ILibWrapper_WebRTC_DataChannel_Close(mDataChannel);
			//ILibWrapper_WebRTC_DataChannel_SendString(mDataChannel, line, strlen(line)); // Send string data over the WebRTC Data Channel
		}
	}
}

#if defined(WIN32)
int _tmain(int argc, _TCHAR* argv[])
#else
int main(int argc, char **argv)
#endif
{
	/* This is to test TLS and TLS Detect
	struct util_cert selfcert;
	struct util_cert selftlscert;
	SSL_CTX* ctx;
	*/

#if defined(WIN32)
	_CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
	SetConsoleCtrlHandler((PHANDLER_ROUTINE)CtrlHandler, TRUE); // Set SIGNAL on windows to listen for Ctrl-C
#elif defined(_POSIX)
	signal(SIGPIPE, SIG_IGN); // Set a SIGNAL on Linux to listen for Ctrl-C

	// Shutdown on Ctrl + C
	signal(SIGINT, BreakSink);
	{
		struct sigaction act; 
		act.sa_handler = SIG_IGN; 
		sigemptyset(&act.sa_mask); 
		act.sa_flags = 0; 
		sigaction(SIGPIPE, &act, NULL);
	}
#endif
	
#if defined(WIN32)
	if (argc>1 && _tcscmp(argv[1], L"STUN")==0)
#else
	if (argc>1 && strcmp(argv[1],"STUN")==0)
#endif
	{
		useStun = 1;
		printf("USING STUN!\r\n");
	}

	chain = ILibCreateChain();	// Create the MicrostackChain, to which we'll attach the WebRTC ConnectionFactory
	mConnectionFactory = ILibWrapper_WebRTC_ConnectionFactory_CreateConnectionFactory(chain, 0); // Create the connection factory, and bind to port "0", to use a random port
	mServer = SimpleRendezvousServer_Create(chain, 5350, &Rendezvous_OnGet, &Rendezvous_OnPost); // Create our simple Rendezvous/HTTP server we'll use to exchange SDP offers, and bind to port 5350

	htmlBodyLength = ILibReadFileFromDiskEx(&htmlBody, "webrtcsample.html");						// This will be the HTML file served if you hit /ACTIVE
	passiveHtmlBodyLength = ILibReadFileFromDiskEx(&passiveHtmlBody, "webrtcpassivesample.html");	// This will be the HTML file served if you hit /PASSIVE
	wshtmlBodyLength = ILibReadFileFromDiskEx(&wshtmlbody, "websocketsample.html");

	printf("Microstack WebRTC Sample Application started.\r\n\r\n");

	// We're actually listening on all interfaces, not just 127.0.0.1
	printf("Browser-initiated connection: http://127.0.0.1:5350/active\r\n");	// This will cause the browser to initiate a WebRTC Connection
	printf("Application-initiated connection: http://127.0.0.1:5350/passive\r\n"); // This will initiate a WebRTC Connection to the browser
	printf("Web-Socket initiated connection: http://127.0.0.1:5350/websocket\r\n"); // This will cause the browser to initiate a websocket connection to exchange WebRTC offers.
	printf("\r\n");
#if defined(_REMOTELOGGING) && defined(_REMOTELOGGINGSERVER)
	printf("Debug logging listening url: http://127.0.0.1:%u\r\n", ILibStartDefaultLogger(chain, 7775));
#endif
	printf("\r\n(Press Ctrl-C to exit)\r\n");
	/* This is to test TLS and TLS Detect
	// Init Certs
	util_mkCert(NULL, &selfcert, 2048, 10000, "localhost", CERTIFICATE_ROOT, NULL);
	util_mkCert(&selfcert, &selftlscert, 2048, 10000, "10.0.0.240", CERTIFICATE_TLS_SERVER, NULL);
	// Init TLS
	ctx = SSL_CTX_new(TLSv1_method());
	SSL_CTX_use_certificate(ctx, selftlscert.x509);
	SSL_CTX_use_PrivateKey(ctx, selftlscert.pkey);
	SimpleRendezvousServer_SetSSL(mServer, ctx);
	*/

	;

	ILibSpawnNormalThread(&Run, NULL); // Spawn a thread to listen for user input
	ILibStartChain(chain); // This will start the Microstack Chain... It will block until ILibStopChain is called

	

	/* This is to test TLS and TLS Detect
	SSL_CTX_free(ctx);
	util_freecert(&selfcert);
	util_freecert(&selftlscert);
	*/


	// This won't execute until the Chain was stopped
	free(htmlBody);
	free(passiveHtmlBody);
	free(wshtmlbody);
	printf("Application exited gracefully.\r\n");
#if defined(WIN32)
	_CrtDumpMemoryLeaks();
#endif
	return(0);
}

