/**********
 This library is free software; you can redistribute it and/or modify it under
 the terms of the GNU Lesser General Public License as published by the
 Free Software Foundation; either version 2.1 of the License, or (at your
 option) any later version. (See <http://www.gnu.org/copyleft/lesser.html>.)

 This library is distributed in the hope that it will be useful, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 more details.

 You should have received a copy of the GNU Lesser General Public License
 along with this library; if not, write to the Free Software Foundation, Inc.,
 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 **********/
// Copyright (c) 1996-2013, Live Networks, Inc.  All rights reserved
// A subclass of "RTSPServer" that creates "ServerMediaSession"s on demand,
// based on whether or not the specified stream name exists as a file
// Implementation

#include "DynamicRTSPServer.hh"
#include <liveMedia.hh>
#include <string.h>

DynamicRTSPServer*
DynamicRTSPServer::createNew(UsageEnvironment& env, Port ourPort,
		UserAuthenticationDatabase* authDatabase,
		unsigned reclamationTestSeconds) {
	int ourSocket = setUpOurSocket(env, ourPort); //建立TCP socket
	if (ourSocket == -1)
		return NULL;

	return new DynamicRTSPServer(env, ourSocket, ourPort, authDatabase,
			reclamationTestSeconds);
}

DynamicRTSPServer::DynamicRTSPServer(UsageEnvironment& env, int ourSocket,
		Port ourPort, UserAuthenticationDatabase* authDatabase,
		unsigned reclamationTestSeconds) :
	RTSPServerSupportingHTTPStreaming(env, ourSocket, ourPort, authDatabase,
			reclamationTestSeconds) {
}

DynamicRTSPServer::~DynamicRTSPServer() {
}

static ServerMediaSession* createNewSMS(UsageEnvironment& env,
		char const* fileName, FILE* fid); // forward

/*
 * DynamicRTSPServer是RTSPServer的子类，继承关系为：DynamicRTSPServer->RTSPServerSupportingHTTPStreaming
 * ->RTSPServer,调用RTSPServer::lookupServerMediaSession,查询streamName对应的session是否存在。若不存在，则
 * 需要创建新的session，并将其加入到链表中否则直接返回已存在的session
 */
ServerMediaSession*
DynamicRTSPServer::lookupServerMediaSession(char const* streamName) {
	// First, check whether the specified "streamName" exists as a local file:
	FILE* fid = fopen(streamName, "rb");
	Boolean fileExists = fid != NULL;

	// Next, check whether we already have a "ServerMediaSession" for this file:
	ServerMediaSession* sms = RTSPServer::lookupServerMediaSession(streamName);	//在成员哈希表中查询
	Boolean smsExists = sms != NULL;

	// Handle the four possibilities for "fileExists" and "smsExists":
	if (!fileExists) {
		if (smsExists) {
			// "sms" was created for a file that no longer exists. Remove it:
			removeServerMediaSession(sms);	//对应的文件已经不存在，从链表中移除session
		}
		return NULL;
	} else {
		if (!smsExists) {
			// Create a new "ServerMediaSession" object for streaming from the named file.
			sms = createNewSMS(envir(), streamName, fid);	//session不存在，则创建(2.2)，根据名字的不同创建不同的session
			addServerMediaSession(sms);						//加入到链表(2.1)
		}
		fclose(fid);
		return sms;
	}
}

// Special code for handling Matroska files:
static char newMatroskaDemuxWatchVariable;
static MatroskaFileServerDemux* demux;
static void onMatroskaDemuxCreation(MatroskaFileServerDemux* newDemux, void* /*clientData*/) {
	demux = newDemux;
	newMatroskaDemuxWatchVariable = 1;
}
// END Special code for handling Matroska files:

#define NEW_SMS(description) do {\
char const* descStr = description\
    ", streamed by the LIVE555 Media Server";\
sms = ServerMediaSession::createNew(env, fileName, fileName, descStr);\
} while(0)
/*
 * 可以看到NEW_SMS("AMR Audio")会创建新的ServerMediaSession,之后马上调用sms->addSubsession()
 * 为这个ServerMediaSession添加一个ServerMediaSbuSession.看起来ServerMediaSession应该可以
 * 添加多个ServerMediaSubSession,但这里并没有这样做。如果可以添加多个ServerMediaSubSession那么
 * ServerMediaSession与流名字所指定的文件是没有关系的，也就是说它不会操作文件，而文件的操作是放在
 *
 * ServerMediaSubSession中的。具体应该是在ServerMediaSubSession的sdpLines()函数中打开。
 */
/*
 * 由于不同的媒体类型，需要创建不同的session，为了便于修改，创建session的代码被放到一个独立的函数
 * createNewSMS中，
 * session的创建被隐藏在宏定义NEW_SMS里。ServerMediaSession::createNew将实例化一个ServerMediaSession对象
 */
static ServerMediaSession* createNewSMS(UsageEnvironment& env,
		char const* fileName, FILE* /*fid*/) {
	// Use the file name extension to determine the type of "ServerMediaSession":
	char const* extension = strrchr(fileName, '.');
	if (extension == NULL)
		return NULL;

	ServerMediaSession* sms = NULL;
	Boolean const reuseSource = False;
	if (strcmp(extension, ".aac") == 0) {
		// Assumed to be an AAC Audio (ADTS format) file:
		NEW_SMS("AAC Audio");
		sms->addSubsession(ADTSAudioFileServerMediaSubsession::createNew(env,
				fileName, reuseSource));
	} else if (strcmp(extension, ".amr") == 0) {
		// Assumed to be an AMR Audio file:
		NEW_SMS("AMR Audio");
		sms->addSubsession(AMRAudioFileServerMediaSubsession::createNew(env,
				fileName, reuseSource));
	} else if (strcmp(extension, ".ac3") == 0) {
		// Assumed to be an AC-3 Audio file:
		NEW_SMS("AC-3 Audio");
		sms->addSubsession(AC3AudioFileServerMediaSubsession::createNew(env,
				fileName, reuseSource));
	} else if (strcmp(extension, ".m4e") == 0) {
		// Assumed to be a MPEG-4 Video Elementary Stream file:
		NEW_SMS("MPEG-4 Video");
		sms->addSubsession(MPEG4VideoFileServerMediaSubsession::createNew(env,
				fileName, reuseSource));
	} else if (strcmp(extension, ".264") == 0) {
		// Assumed to be a H.264 Video Elementary Stream file:
		NEW_SMS("H.264 Video");
		OutPacketBuffer::maxSize = 100000; // allow for some possibly large H.264 frames
		sms->addSubsession(H264VideoFileServerMediaSubsession::createNew(env,
				fileName, reuseSource));
	} else if (strcmp(extension, ".mp3") == 0) {
		// Assumed to be a MPEG-1 or 2 Audio file:
		NEW_SMS("MPEG-1 or 2 Audio");
		// To stream using 'ADUs' rather than raw MP3 frames, uncomment the following:
		//#define STREAM_USING_ADUS 1
		// To also reorder ADUs before streaming, uncomment the following:
		//#define INTERLEAVE_ADUS 1
		// (For more information about ADUs and interleaving,
		//  see <http://www.live555.com/rtp-mp3/>)
		Boolean useADUs = False;
		Interleaving* interleaving = NULL;
#ifdef STREAM_USING_ADUS
		useADUs = True;
#ifdef INTERLEAVE_ADUS
		unsigned char interleaveCycle[] = {0,2,1,3}; // or choose your own...
		unsigned const interleaveCycleSize
		= (sizeof interleaveCycle)/(sizeof (unsigned char));
		interleaving = new Interleaving(interleaveCycleSize, interleaveCycle);
#endif
#endif
		sms->addSubsession(MP3AudioFileServerMediaSubsession::createNew(env,
				fileName, reuseSource, useADUs, interleaving));
	} else if (strcmp(extension, ".mpg") == 0) {
		// Assumed to be a MPEG-1 or 2 Program Stream (audio+video) file:
		NEW_SMS("MPEG-1 or 2 Program Stream");
		MPEG1or2FileServerDemux* demux = MPEG1or2FileServerDemux::createNew(
				env, fileName, reuseSource);
		sms->addSubsession(demux->newVideoServerMediaSubsession());
		sms->addSubsession(demux->newAudioServerMediaSubsession());
	} else if (strcmp(extension, ".vob") == 0) {
		// Assumed to be a VOB (MPEG-2 Program Stream, with AC-3 audio) file:
		NEW_SMS("VOB (MPEG-2 video with AC-3 audio)");
		MPEG1or2FileServerDemux* demux = MPEG1or2FileServerDemux::createNew(
				env, fileName, reuseSource);
		sms->addSubsession(demux->newVideoServerMediaSubsession());
		sms->addSubsession(demux->newAC3AudioServerMediaSubsession());
	} else if (strcmp(extension, ".ts") == 0) {
		// Assumed to be a MPEG Transport Stream file:
		// Use an index file name that's the same as the TS file name, except with ".tsx":
		unsigned indexFileNameLen = strlen(fileName) + 2; // allow for trailing "x\0"
		char* indexFileName = new char[indexFileNameLen];
		sprintf(indexFileName, "%sx", fileName);
		NEW_SMS("MPEG Transport Stream");
		sms->addSubsession(MPEG2TransportFileServerMediaSubsession::createNew(
				env, fileName, indexFileName, reuseSource));
		delete[] indexFileName;
	} else if (strcmp(extension, ".wav") == 0) {
		// Assumed to be a WAV Audio file:
		NEW_SMS("WAV Audio Stream");
		// To convert 16-bit PCM data to 8-bit u-law, prior to streaming,
		// change the following to True:
		Boolean convertToULaw = False;
		sms->addSubsession(WAVAudioFileServerMediaSubsession::createNew(env,
				fileName, reuseSource, convertToULaw));
	} else if (strcmp(extension, ".dv") == 0) {
		// Assumed to be a DV Video file
		// First, make sure that the RTPSinks' buffers will be large enough to handle the huge size of DV frames (as big as 288000).
		OutPacketBuffer::maxSize = 300000;

		NEW_SMS("DV Video");
		sms->addSubsession(DVVideoFileServerMediaSubsession::createNew(env,
				fileName, reuseSource));
	} else if (strcmp(extension, ".mkv") == 0 || strcmp(extension, ".webm")
			== 0) {
		// Assumed to be a Matroska file (note that WebM ('.webm') files are also Matroska files)
		NEW_SMS("Matroska video+audio+(optional)subtitles");

		// Create a Matroska file server demultiplexor for the specified file.  (We enter the event loop to wait for this to complete.)
		newMatroskaDemuxWatchVariable = 0;
		MatroskaFileServerDemux::createNew(env, fileName,
				onMatroskaDemuxCreation, NULL);
		env.taskScheduler().doEventLoop(&newMatroskaDemuxWatchVariable);

		ServerMediaSubsession* smss;
		while ((smss = demux->newServerMediaSubsession()) != NULL) {
			sms->addSubsession(smss);
		}
	}

	return sms;
}
