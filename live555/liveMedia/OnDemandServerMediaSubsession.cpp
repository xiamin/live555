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
// "liveMedia"
// Copyright (c) 1996-2013 Live Networks, Inc.  All rights reserved.
// A 'ServerMediaSubsession' object that creates new, unicast, "RTPSink"s
// on demand.
// Implementation

#include "OnDemandServerMediaSubsession.hh"
#include <GroupsockHelper.hh>

OnDemandServerMediaSubsession::OnDemandServerMediaSubsession(
    UsageEnvironment& env, Boolean reuseFirstSource,
    portNumBits initialPortNum) :
    ServerMediaSubsession(env), fSDPLines(NULL), fReuseFirstSource(
        reuseFirstSource), fInitialPortNum(initialPortNum),
    fLastStreamToken(NULL)
{
    fDestinationsHashTable = HashTable::create(ONE_WORD_HASH_KEYS);
    gethostname(fCNAME, sizeof fCNAME);
    fCNAME[sizeof fCNAME - 1] = '\0'; // just in case
}

OnDemandServerMediaSubsession::~OnDemandServerMediaSubsession()
{
    delete[] fSDPLines;

    // Clean out the destinations hash table:
    while (1)
    {
        Destinations* destinations =
            (Destinations*) (fDestinationsHashTable->RemoveNext());
        if (destinations == NULL)
            break;
        delete destinations;
    }
    delete fDestinationsHashTable;
}

char const*
OnDemandServerMediaSubsession::sdpLines()
{
    if (fSDPLines == NULL)
    {
        // We need to construct a set of SDP lines that describe this
        // subsession (as a unicast stream).  To do so, we first create
        // dummy (unused) source and "RTPSink" objects,
        // whose parameters we use for the SDP lines:
        unsigned estBitrate;
        FramedSource* inputSource = createNewStreamSource(0, estBitrate);
        if (inputSource == NULL)
            return NULL; // file not found

        struct in_addr dummyAddr;
        dummyAddr.s_addr = 0;
        Groupsock dummyGroupsock(envir(), dummyAddr, 0, 0);
        unsigned char rtpPayloadType = 96 + trackNumber() - 1; // if dynamic
        RTPSink* dummyRTPSink = createNewRTPSink(&dummyGroupsock,
                                rtpPayloadType, inputSource);

        setSDPLinesFromRTPSink(dummyRTPSink, inputSource, estBitrate);
        Medium::close(dummyRTPSink);
        closeStreamSource(inputSource);
    }

    return fSDPLines;
}

/*
 * getStreamParameters是定义在ServerMediaSubsession类中的纯虚函数，其实现在子类
 * OnDemandServerMediaSubsession中。这个函数中将完成source，RTPSink的创建工作，
 * 并将其与客户端的映射关系保存下来
 */
void OnDemandServerMediaSubsession::getStreamParameters(
    unsigned clientSessionId, netAddressBits clientAddress,
    Port const& clientRTPPort, Port const& clientRTCPPort,
    int tcpSocketNum, unsigned char rtpChannelId,
    unsigned char rtcpChannelId, netAddressBits& destinationAddress,
    u_int8_t& /*destinationTTL*/, Boolean& isMulticast,
    Port& serverRTPPort, Port& serverRTCPPort, void*& streamToken)
{
    if (destinationAddress == 0)
        destinationAddress = clientAddress;
    struct in_addr destinationAddr;
    destinationAddr.s_addr = destinationAddress;
    isMulticast = False;

    if (fLastStreamToken != NULL && fReuseFirstSource)
    {
        //当fReuseFirstSource参数为True时，不需要再创建source, sink, groupsock等实例，只需要记录客户端的地址即可

        // Special case: Rather than creating a new 'StreamState',
        // we reuse the one that we've already created:
        serverRTPPort = ((StreamState*) fLastStreamToken)->serverRTPPort();
        serverRTCPPort = ((StreamState*) fLastStreamToken)->serverRTCPPort();
        ++((StreamState*) fLastStreamToken)->referenceCount();	//增加引用计数
        streamToken = fLastStreamToken;
    }
    else
    {
        //正常情况下，创建一个新的media sourc
        // Normal case: Create a new media source:
        unsigned streamBitrate;
        /*
         * 创建source，还记得在处理DESCRIBE命令时，也创建过吗？是的，那是在OnDemandServerMediaSubsession::sdpLines()
         * 函数中，但参数clientSessionId为0. createNewSource函数的具体实现参见先前的文章中关于DESCRIBE命令的处理流程
         */
        FramedSource* mediaSource = createNewStreamSource(clientSessionId,
                                    streamBitrate);

        // Create 'groupsock' and 'sink' objects for the destination,
        // using previously unused server port numbers:
        RTPSink* rtpSink;
        BasicUDPSink* udpSink;
        Groupsock* rtpGroupsock;
        Groupsock* rtcpGroupsock;
        portNumBits serverPortNum;
        if (clientRTCPPort.num() == 0)
        {
            //使用RAW UDP传输，当然就不用使用RTCP了
            // We're streaming raw UDP (not RTP). Create a single groupsock:
            NoReuse dummy(envir()); // ensures that we skip over ports that are already in use
            for (serverPortNum = fInitialPortNum;; ++serverPortNum)
            {
                struct in_addr dummyAddr;
                dummyAddr.s_addr = 0;

                serverRTPPort = serverPortNum;
                rtpGroupsock = new Groupsock(envir(), dummyAddr, serverRTPPort,
                                             255);
                if (rtpGroupsock->socketNum() >= 0)
                    break; // success
            }

            rtcpGroupsock = NULL;
            rtpSink = NULL;
            udpSink = BasicUDPSink::createNew(envir(), rtpGroupsock);
        }
        else
        {

            //创建一对groupsock实例，分别用与传输RTP、RTCP
            /*
             * RTP、RTCP的端口号是相邻的，并且RTP端口号为偶数。初始化端口fInitialPortNum = 6970
             * 这是OnDemandServerMediaSubsession构造函数的缺省参数
             */
            // Normal case: We're streaming RTP (over UDP or TCP).  Create a pair of
            // groupsocks (RTP and RTCP), with adjacent port numbers (RTP port number even):
            NoReuse dummy(envir()); // ensures that we skip over ports that are already in use
            for (portNumBits serverPortNum = fInitialPortNum;; serverPortNum
                    += 2)
            {
                struct in_addr dummyAddr;
                dummyAddr.s_addr = 0;

                serverRTPPort = serverPortNum;
                rtpGroupsock = new Groupsock(envir(), dummyAddr, serverRTPPort,
                                             255);
                if (rtpGroupsock->socketNum() < 0)
                {
                    delete rtpGroupsock;
                    continue; // try again
                }

                serverRTCPPort = serverPortNum + 1;	//与RTP端口号相邻的
                rtcpGroupsock = new Groupsock(envir(), dummyAddr,
                                              serverRTCPPort, 255);
                if (rtcpGroupsock->socketNum() < 0)
                {
                    delete rtpGroupsock;
                    delete rtcpGroupsock;
                    continue; // try again
                }

                break; // success
            }
            //创建RTPSink，与source类似，在处理DESCRIBE命令进行过，具体过程参见DESCRIBE命令的处理流程
            unsigned char rtpPayloadType = 96 + trackNumber() - 1; // if dynamic
            rtpSink = createNewRTPSink(rtpGroupsock, rtpPayloadType,
                                       mediaSource);
            udpSink = NULL;
        }

        // Turn off the destinations for each groupsock.  They'll get set later
        // (unless TCP is used instead):
        if (rtpGroupsock != NULL)
            rtpGroupsock->removeAllDestinations();
        if (rtcpGroupsock != NULL)
            rtcpGroupsock->removeAllDestinations();

        //重新配置发送RTP的socket缓冲区大小
        if (rtpGroupsock != NULL)
        {
            // Try to use a big send buffer for RTP -  at least 0.1 second of
            // specified bandwidth and at least 50 KB
            unsigned rtpBufSize = streamBitrate * 25 / 2; // 1 kbps * 0.1 s = 12.5 bytes
            if (rtpBufSize < 50 * 1024)
                rtpBufSize = 50 * 1024;
            increaseSendBufferTo(envir(), rtpGroupsock->socketNum(), rtpBufSize);
        }

        /*
         * 建立流的状态对象(stream token),其他包括sink、source、groupsock等的对应关系
         * 注意，live555中定义了两个StreamState结构，这里的StreamState定义为一个类。在RTSPServer
         * 中，定义了一个内部结构体StreamState,其streamToken成员指向此处的StreamState实例
         */

        // Set up the state of the stream.  The stream will get started later:
        streamToken = fLastStreamToken = new StreamState(*this, serverRTPPort,
                serverRTCPPort, rtpSink, udpSink, streamBitrate, mediaSource,
                rtpGroupsock, rtcpGroupsock);
    }

    //这里定义了类Destinations来保存目的地址、RTP端口、RTCP端口，并将其与对应的clientSessionId
    //保存到哈希表fDestinationHashTable中，这个哈希表定义在OnDemandServerMediaSubsession中
    // Record these destinations as being for this client session id:
    Destinations* destinations;
    if (tcpSocketNum < 0)   // UDP
    {
        destinations = new Destinations(destinationAddr, clientRTPPort,
                                        clientRTCPPort);
    }
    else     // TCP
    {
        destinations = new Destinations(tcpSocketNum, rtpChannelId,
                                        rtcpChannelId);
    }
    fDestinationsHashTable->Add((char const*) clientSessionId, destinations);
}

void OnDemandServerMediaSubsession::startStream(
    unsigned clientSessionId,
    void* streamToken,
    TaskFunc* rtcpRRHandler,
    void* rtcpRRHandlerClientData,
    unsigned short& rtpSeqNum,
    unsigned& rtpTimestamp,
    ServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
    void* serverRequestAlternativeByteHandlerClientData)
{
    StreamState* streamState = (StreamState*) streamToken;
    Destinations* destinations =
        (Destinations*) (fDestinationsHashTable->Lookup(
                             (char const*) clientSessionId));
    if (streamState != NULL)
    {
        streamState->startPlaying(destinations, rtcpRRHandler,
                                  rtcpRRHandlerClientData, serverRequestAlternativeByteHandler,
                                  serverRequestAlternativeByteHandlerClientData);
        RTPSink* rtpSink = streamState->rtpSink(); // alias
        if (rtpSink != NULL)
        {
            rtpSeqNum = rtpSink->currentSeqNo();
            rtpTimestamp = rtpSink->presetNextTimestamp();
        }
    }
}

void OnDemandServerMediaSubsession::pauseStream(unsigned /*clientSessionId*/,
        void* streamToken)
{
    // Pausing isn't allowed if multiple clients are receiving data from
    // the same source:
    if (fReuseFirstSource)
        return;

    StreamState* streamState = (StreamState*) streamToken;
    if (streamState != NULL)
        streamState->pause();
}

void OnDemandServerMediaSubsession::seekStream(unsigned /*clientSessionId*/,
        void* streamToken, double& seekNPT, double streamDuration,
        u_int64_t& numBytes)
{
    numBytes = 0; // by default: unknown

    // Seeking isn't allowed if multiple clients are receiving data from the same source:
    if (fReuseFirstSource)
        return;

    StreamState* streamState = (StreamState*) streamToken;
    if (streamState != NULL && streamState->mediaSource() != NULL)
    {
        seekStreamSource(streamState->mediaSource(), seekNPT, streamDuration,
                         numBytes);

        streamState->startNPT() = (float) seekNPT;
        RTPSink* rtpSink = streamState->rtpSink(); // alias
        if (rtpSink != NULL)
            rtpSink->resetPresentationTimes();
    }
}

void OnDemandServerMediaSubsession::seekStream(unsigned /*clientSessionId*/,
        void* streamToken, char*& absStart, char*& absEnd)
{
    // Seeking isn't allowed if multiple clients are receiving data from the same source:
    if (fReuseFirstSource)
        return;

    StreamState* streamState = (StreamState*) streamToken;
    if (streamState != NULL && streamState->mediaSource() != NULL)
    {
        seekStreamSource(streamState->mediaSource(), absStart, absEnd);
    }
}

void OnDemandServerMediaSubsession::nullSeekStream(
    unsigned /*clientSessionId*/, void* streamToken)
{
    StreamState* streamState = (StreamState*) streamToken;
    if (streamState != NULL && streamState->mediaSource() != NULL)
    {
        // Because we're not seeking here, get the current NPT, and remember it as the new 'start' NPT:
        streamState->startNPT() = getCurrentNPT(streamToken);
        RTPSink* rtpSink = streamState->rtpSink(); // alias
        if (rtpSink != NULL)
            rtpSink->resetPresentationTimes();
    }
}

void OnDemandServerMediaSubsession::setStreamScale(
    unsigned /*clientSessionId*/, void* streamToken, float scale)
{
    // Changing the scale factor isn't allowed if multiple clients are receiving data
    // from the same source:
    if (fReuseFirstSource)
        return;

    StreamState* streamState = (StreamState*) streamToken;
    if (streamState != NULL && streamState->mediaSource() != NULL)
    {
        setStreamSourceScale(streamState->mediaSource(), scale);
    }
}

float OnDemandServerMediaSubsession::getCurrentNPT(void* streamToken)
{
    do
    {
        if (streamToken == NULL)
            break;

        StreamState* streamState = (StreamState*) streamToken;
        RTPSink* rtpSink = streamState->rtpSink();
        if (rtpSink == NULL)
            break;

        return streamState->startNPT()
               + (rtpSink->mostRecentPresentationTime().tv_sec
                  - rtpSink->initialPresentationTime().tv_sec)
               + (rtpSink->mostRecentPresentationTime().tv_sec
                  - rtpSink->initialPresentationTime().tv_sec)
               / 1000000.0f;
    }
    while (0);

    return 0.0;
}

FramedSource* OnDemandServerMediaSubsession::getStreamSource(void* streamToken)
{
    if (streamToken == NULL)
        return NULL;

    StreamState* streamState = (StreamState*) streamToken;
    return streamState->mediaSource();
}

void OnDemandServerMediaSubsession::deleteStream(unsigned clientSessionId,
        void*& streamToken)
{
    StreamState* streamState = (StreamState*) streamToken;

    // Look up (and remove) the destinations for this client session:
    Destinations* destinations =
        (Destinations*) (fDestinationsHashTable->Lookup(
                             (char const*) clientSessionId));
    if (destinations != NULL)
    {
        fDestinationsHashTable->Remove((char const*) clientSessionId);

        // Stop streaming to these destinations:
        if (streamState != NULL)
            streamState->endPlaying(destinations);
    }

    // Delete the "StreamState" structure if it's no longer being used:
    if (streamState != NULL)
    {
        if (streamState->referenceCount() > 0)
            --streamState->referenceCount();
        if (streamState->referenceCount() == 0)
        {
            delete streamState;
            streamToken = NULL;
        }
    }

    // Finally, delete the destinations themselves:
    delete destinations;
}

char const* OnDemandServerMediaSubsession::getAuxSDPLine(RTPSink* rtpSink,
        FramedSource* /*inputSource*/)
{
    // Default implementation:
    return rtpSink == NULL ? NULL : rtpSink->auxSDPLine();
}

void OnDemandServerMediaSubsession::seekStreamSource(
    FramedSource* /*inputSource*/, double& /*seekNPT*/,
    double /*streamDuration*/, u_int64_t& numBytes)
{
    // Default implementation: Do nothing
}

void OnDemandServerMediaSubsession::seekStreamSource(
    FramedSource* /*inputSource*/, char*& absStart, char*& absEnd)
{
    // Default implementation: do nothing (but delete[] and assign "absStart" and "absEnd" to NULL, to show that we don't handle this)
    delete[] absStart;
    absStart = NULL;
    delete[] absEnd;
    absEnd = NULL;
}

void OnDemandServerMediaSubsession::setStreamSourceScale(
    FramedSource* /*inputSource*/, float /*scale*/)
{
    // Default implementation: Do nothing
}

void OnDemandServerMediaSubsession::closeStreamSource(FramedSource *inputSource)
{
    Medium::close(inputSource);
}

void OnDemandServerMediaSubsession::setSDPLinesFromRTPSink(RTPSink* rtpSink,
        FramedSource* inputSource, unsigned estBitrate)
{
    if (rtpSink == NULL)
        return;

    char const* mediaType = rtpSink->sdpMediaType();
    unsigned char rtpPayloadType = rtpSink->rtpPayloadType();
    AddressString ipAddressStr(fServerAddressForSDP);
    char* rtpmapLine = rtpSink->rtpmapLine();
    char const* rangeLine = rangeSDPLine();
    char const* auxSDPLine = getAuxSDPLine(rtpSink, inputSource);
    if (auxSDPLine == NULL)
        auxSDPLine = "";

    char const* const sdpFmt = "m=%s %u RTP/AVP %d\r\n"
                               "c=IN IP4 %s\r\n"
                               "b=AS:%u\r\n"
                               "%s"
                               "%s"
                               "%s"
                               "a=control:%s\r\n";
    unsigned sdpFmtSize = strlen(sdpFmt) + strlen(mediaType) + 5
                          /* max short len */+ 3 /* max char len */
                          + strlen(ipAddressStr.val()) + 20 /* max int len */
                          + strlen(rtpmapLine) + strlen(rangeLine) + strlen(auxSDPLine) + strlen(
                              trackId());
    char* sdpLines = new char[sdpFmtSize];
    sprintf(sdpLines, sdpFmt, mediaType, // m= <media>
            fPortNumForSDP, // m= <port>
            rtpPayloadType, // m= <fmt list>
            ipAddressStr.val(), // c= address
            estBitrate, // b=AS:<bandwidth>
            rtpmapLine, // a=rtpmap:... (if present)
            rangeLine, // a=range:... (if present)
            auxSDPLine, // optional extra SDP line
            trackId()); // a=control:<track-id>
    delete[] (char*) rangeLine;
    delete[] rtpmapLine;

    fSDPLines = strDup(sdpLines);
    delete[] sdpLines;
}

////////// StreamState implementation //////////

static void afterPlayingStreamState(void* clientData)
{
    StreamState* streamState = (StreamState*) clientData;
    if (streamState->streamDuration() == 0.0)
    {
        // When the input stream ends, tear it down.  This will cause a RTCP "BYE"
        // to be sent to each client, teling it that the stream has ended.
        // (Because the stream didn't have a known duration, there was no other
        //  way for clients to know when the stream ended.)
        streamState->reclaim();
    }
    // Otherwise, keep the stream alive, in case a client wants to
    // subsequently re-play the stream starting from somewhere other than the end.
    // (This can be done only on streams that have a known duration.)
}

StreamState::StreamState(OnDemandServerMediaSubsession& master,
                         Port const& serverRTPPort, Port const& serverRTCPPort,
                         RTPSink* rtpSink, BasicUDPSink* udpSink, unsigned totalBW,
                         FramedSource* mediaSource, Groupsock* rtpGS, Groupsock* rtcpGS) :
    fMaster(master), fAreCurrentlyPlaying(False), fReferenceCount(1),
    fServerRTPPort(serverRTPPort), fServerRTCPPort(serverRTCPPort),
    fRTPSink(rtpSink), fUDPSink(udpSink), fStreamDuration(
        master.duration()), fTotalBW(totalBW),
    fRTCPInstance(NULL) /* created later */, fMediaSource(mediaSource),
    fStartNPT(0.0), fRTPgs(rtpGS), fRTCPgs(rtcpGS)
{
}

StreamState::~StreamState()
{
    reclaim();
}

void StreamState::startPlaying(
    Destinations* dests,
    TaskFunc* rtcpRRHandler,
    void* rtcpRRHandlerClientData,
    ServerRequestAlternativeByteHandler* serverRequestAlternativeByteHandler,
    void* serverRequestAlternativeByteHandlerClientData)
{
    if (dests == NULL)
        return;
    //创建RTCPInstance 实例
    if (fRTCPInstance == NULL && fRTPSink != NULL)
    {
        // Create (and start) a 'RTCP instance' for this RTP sink:
        fRTCPInstance
            = RTCPInstance::createNew(fRTPSink->envir(), fRTCPgs, fTotalBW,
                                      (unsigned char*) fMaster.fCNAME, fRTPSink, NULL /* we're a server */);
        // Note: This starts RTCP running automatically
    }

    if (dests->isTCP)
    {
        //使用TCP 传输rtp.rtcp
        // Change RTP and RTCP to use the TCP socket instead of UDP:
        if (fRTPSink != NULL)
        {
            fRTPSink->addStreamSocket(dests->tcpSocketNum, dests->rtpChannelId);
            fRTPSink->setServerRequestAlternativeByteHandler(
                dests->tcpSocketNum, serverRequestAlternativeByteHandler,
                serverRequestAlternativeByteHandlerClientData);
            // So that we continue to handle RTSP commands from the client
        }
        if (fRTCPInstance != NULL)
        {
            fRTCPInstance->addStreamSocket(dests->tcpSocketNum,
                                           dests->rtcpChannelId);
            fRTCPInstance->setSpecificRRHandler(dests->tcpSocketNum,
                                                dests->rtcpChannelId, rtcpRRHandler,
                                                rtcpRRHandlerClientData);
        }
    }
    else
    {
        //使用UDP 传输RTP. RTCP
        // Tell the RTP and RTCP 'groupsocks' about this destination
        // (in case they don't already have it):
        if (fRTPgs != NULL)
            fRTPgs->addDestination(dests->addr, dests->rtpPort);
        if (fRTCPgs != NULL)
            fRTCPgs->addDestination(dests->addr, dests->rtcpPort);
        if (fRTCPInstance != NULL)
        {
            fRTCPInstance->setSpecificRRHandler(dests->addr.s_addr,
                                                dests->rtcpPort, rtcpRRHandler, rtcpRRHandlerClientData);
        }
    }

    if (fRTCPInstance != NULL)
    {
        // Hack: Send an initial RTCP "SR" packet, before the initial RTP packet, so that receivers will (likely) be able to
        // get RTCP-synchronized presentation times immediately:
        fRTCPInstance->sendReport();
    }

    // 下面调用sink  上 的 startPlaying 函数
    if (!fAreCurrentlyPlaying && fMediaSource != NULL)
    {
        if (fRTPSink != NULL) /*使用RTP 协议传输数据*/
        {
            fRTPSink->startPlaying(*fMediaSource, afterPlayingStreamState, this);
            fAreCurrentlyPlaying = True;
        }
        else if (fUDPSink != NULL) /*裸的UDP 数据包，不使用RTP 协议*/
        {
            fUDPSink->startPlaying(*fMediaSource, afterPlayingStreamState, this);
            fAreCurrentlyPlaying = True;
        }
    }
}

void StreamState::pause()
{
    if (fRTPSink != NULL)
        fRTPSink->stopPlaying();
    if (fUDPSink != NULL)
        fUDPSink->stopPlaying();
    fAreCurrentlyPlaying = False;
}

void StreamState::endPlaying(Destinations* dests)
{
#if 0
    // The following code is temporarily disabled, because it erroneously sends RTCP "BYE"s to all clients if multiple
    // clients are streaming from the samedata source (i.e., if "reuseFirstSource" is True).  It will be fixed for real later.
    if (fRTCPInstance != NULL)
    {
        // Hack: Explicitly send a RTCP "BYE" packet now, because the code below will prevent that from happening later,
        // when "fRTCPInstance" gets deleted:
        fRTCPInstance->sendBYE();
    }
#endif

    if (dests->isTCP)
    {
        if (fRTPSink != NULL)
        {
            fRTPSink->setServerRequestAlternativeByteHandler(
                dests->tcpSocketNum, NULL, NULL);
            fRTPSink->removeStreamSocket(dests->tcpSocketNum,
                                         dests->rtpChannelId);
        }
        if (fRTCPInstance != NULL)
        {
            fRTCPInstance->removeStreamSocket(dests->tcpSocketNum,
                                              dests->rtcpChannelId);
            fRTCPInstance->unsetSpecificRRHandler(dests->tcpSocketNum,
                                                  dests->rtcpChannelId);
        }
    }
    else
    {
        // Tell the RTP and RTCP 'groupsocks' to stop using these destinations:
        if (fRTPgs != NULL)
            fRTPgs->removeDestination(dests->addr, dests->rtpPort);
        if (fRTCPgs != NULL)
            fRTCPgs->removeDestination(dests->addr, dests->rtcpPort);
        if (fRTCPInstance != NULL)
        {
            fRTCPInstance->unsetSpecificRRHandler(dests->addr.s_addr,
                                                  dests->rtcpPort);
        }
    }
}

void StreamState::reclaim()
{
    // Delete allocated media objects
    Medium::close(fRTCPInstance) /* will send a RTCP BYE */;
    fRTCPInstance = NULL;
    Medium::close(fRTPSink);
    fRTPSink = NULL;
    Medium::close(fUDPSink);
    fUDPSink = NULL;

    fMaster.closeStreamSource(fMediaSource);
    fMediaSource = NULL;
    if (fMaster.fLastStreamToken == this)
        fMaster.fLastStreamToken = NULL;

    delete fRTPgs;
    fRTPgs = NULL;
    delete fRTCPgs;
    fRTCPgs = NULL;
}
