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
// RTP sink for a common kind of payload format: Those which pack multiple,
// complete codec frames (as many as possible) into each RTP packet.
// Implementation

#include "MultiFramedRTPSink.hh"
#include "GroupsockHelper.hh"

////////// MultiFramedRTPSink //////////

void MultiFramedRTPSink::setPacketSizes(unsigned preferredPacketSize,
                                        unsigned maxPacketSize)
{
    if (preferredPacketSize > maxPacketSize || preferredPacketSize == 0) return;
    // sanity check

    delete fOutBuf;
    fOutBuf = new OutPacketBuffer(preferredPacketSize, maxPacketSize);
    fOurMaxPacketSize = maxPacketSize; // save value, in case subclasses need it
}

MultiFramedRTPSink::MultiFramedRTPSink(UsageEnvironment& env,
                                       Groupsock* rtpGS,
                                       unsigned char rtpPayloadType,
                                       unsigned rtpTimestampFrequency,
                                       char const* rtpPayloadFormatName,
                                       unsigned numChannels)
    : RTPSink(env, rtpGS, rtpPayloadType, rtpTimestampFrequency,
              rtpPayloadFormatName, numChannels),
    fOutBuf(NULL), fCurFragmentationOffset(0), fPreviousFrameEndedFragmentation(False),
    fOnSendErrorFunc(NULL), fOnSendErrorData(NULL)
{
    setPacketSizes(1000, 1448);
    // Default max packet size (1500, minus allowance for IP, UDP, UMTP headers)
    // (Also, make it a multiple of 4 bytes, just in case that matters.)
}

MultiFramedRTPSink::~MultiFramedRTPSink()
{
    delete fOutBuf;
}

void MultiFramedRTPSink
::doSpecialFrameHandling(unsigned /*fragmentationOffset*/,
                         unsigned char* /*frameStart*/,
                         unsigned /*numBytesInFrame*/,
                         struct timeval framePresentationTime,
                         unsigned /*numRemainingBytes*/)
{
    // default implementation: If this is the first frame in the packet,
    // use its presentationTime for the RTP timestamp:
    if (isFirstFrameInPacket())
    {
        setTimestamp(framePresentationTime);
    }
}

Boolean MultiFramedRTPSink::allowFragmentationAfterStart() const
{
    return False; // by default
}

Boolean MultiFramedRTPSink::allowOtherFramesAfterLastFragment() const
{
    return False; // by default
}

Boolean MultiFramedRTPSink
::frameCanAppearAfterPacketStart(unsigned char const* /*frameStart*/,
                                 unsigned /*numBytesInFrame*/) const
{
    return True; // by default
}

unsigned MultiFramedRTPSink::specialHeaderSize() const
{
    // default implementation: Assume no special header:
    return 0;
}

unsigned MultiFramedRTPSink::frameSpecificHeaderSize() const
{
    // default implementation: Assume no frame-specific header:
    return 0;
}

unsigned MultiFramedRTPSink::computeOverflowForNewFrame(unsigned newFrameSize) const
{
    // default implementation: Just call numOverflowBytes()
    return fOutBuf->numOverflowBytes(newFrameSize);
}

void MultiFramedRTPSink::setMarkerBit()
{
    unsigned rtpHdr = fOutBuf->extractWord(0);
    rtpHdr |= 0x00800000;
    fOutBuf->insertWord(rtpHdr, 0);
}

void MultiFramedRTPSink::setTimestamp(struct timeval framePresentationTime)
{
    // First, convert the presentation time to a 32-bit RTP timestamp:
    fCurrentTimestamp = convertToRTPTimestamp(framePresentationTime);

    // Then, insert it into the RTP packet:
    fOutBuf->insertWord(fCurrentTimestamp, fTimestampPosition);
}

void MultiFramedRTPSink::setSpecialHeaderWord(unsigned word,
        unsigned wordPosition)
{
    fOutBuf->insertWord(word, fSpecialHeaderPosition + 4*wordPosition);
}

void MultiFramedRTPSink::setSpecialHeaderBytes(unsigned char const* bytes,
        unsigned numBytes,
        unsigned bytePosition)
{
    fOutBuf->insert(bytes, numBytes, fSpecialHeaderPosition + bytePosition);
}

void MultiFramedRTPSink::setFrameSpecificHeaderWord(unsigned word,
        unsigned wordPosition)
{
    fOutBuf->insertWord(word, fCurFrameSpecificHeaderPosition + 4*wordPosition);
}

void MultiFramedRTPSink::setFrameSpecificHeaderBytes(unsigned char const* bytes,
        unsigned numBytes,
        unsigned bytePosition)
{
    fOutBuf->insert(bytes, numBytes, fCurFrameSpecificHeaderPosition + bytePosition);
}

void MultiFramedRTPSink::setFramePadding(unsigned numPaddingBytes)
{
    if (numPaddingBytes > 0)
    {
        // Add the padding bytes (with the last one being the padding size):
        unsigned char paddingBuffer[255]; //max padding
        memset(paddingBuffer, 0, numPaddingBytes);
        paddingBuffer[numPaddingBytes-1] = numPaddingBytes;
        fOutBuf->enqueue(paddingBuffer, numPaddingBytes);

        // Set the RTP padding bit:
        unsigned rtpHdr = fOutBuf->extractWord(0);
        rtpHdr |= 0x20000000;
        fOutBuf->insertWord(rtpHdr, 0);
    }
}

Boolean MultiFramedRTPSink::continuePlaying()
{
    // Send the first packet.
    // (This will also schedule any future sends.)
    buildAndSendPacket(True);
    return True;
}

void MultiFramedRTPSink::stopPlaying()
{
    fOutBuf->resetPacketStart();
    fOutBuf->resetOffset();
    fOutBuf->resetOverflowData();

    // Then call the default "stopPlaying()" function:
    MediaSink::stopPlaying();
}

/*
  *bindAndSendPacket函数中，完成RTP头的准备工作。可以看到RTP头是非常简单的，RTP头中的序号
  *非常重要，客户端需要据此进行RTP包的重排序操作。RTP包内容存放在一个
  *OutPacketBuffer类型的fOutBuf成员变量中，OutPacketBuffer类的细节在文章的最后还会讨论。
  * 在RTP头中预留了一些空间没有进行实际的填充，这个工作将在doSpecialFrameHandling中
  * 进行，后面会有讨论。进一步的工作，在packFrame函数中进行，它将为RTP包填充数据。
  */
void MultiFramedRTPSink::buildAndSendPacket(Boolean isFirstPacket)
{
    fIsFirstPacket = isFirstPacket;

    //设置RTP 头，注意，接收端需要根据RTP 包的序号fSeqNo来重新排序
    // Set up the RTP header:
    unsigned rtpHdr = 0x80000000; // RTP version 2; marker ('M') bit not set (by default; it can be set later)
    rtpHdr |= (fRTPPayloadType<<16);	/*负载类型*/
    rtpHdr |= fSeqNo; /* sequence number/*序列号*/
    fOutBuf->enqueueWord(rtpHdr);

    //保留一个4 bytes 空间，用于设置time stamp
    // Note where the RTP timestamp will go.
    // (We can't fill this in until we start packing payload frames.)
    fTimestampPosition = fOutBuf->curPacketSize();
    fOutBuf->skipBytes(4); // leave a hole for the timestamp

    fOutBuf->enqueueWord(SSRC());/*跟RTP相关，作用暂不清楚*/

    // Allow for a special, payload-format-specific header following the
    // RTP header:
    fSpecialHeaderPosition = fOutBuf->curPacketSize();
    /*
      * specicalHeaderSize在MultiFrameRTPSink 中的默认实现返回0
      * 对于H264的实现不需要处理这个字段
      */
    fSpecialHeaderSize = specialHeaderSize();
    fOutBuf->skipBytes(fSpecialHeaderSize);/*预留空间*/


    //尽可能多的frames到packet中
    // Begin packing as many (complete) frames into the packet as we can:
    fTotalFrameSpecificHeaderSizes = 0;
    fNoFramesLeft = False;
    fNumFramesUsedSoFar = 0;
    packFrame();
}
/*
 *packFrame函数需要处理两种情况
 * 1. buffer中存在为发送的数据(overflowdata),这时可以将调用afterGettingFrame1函数进行后续处理工作
 * 2. buffer不存在数据，这是需要调用source上的getNextFrame函数获取数据。getNextFrame调用时，参数中
 *	  有两个回调函数：afterGettingFrame函数将在获取到数据后调用，其中只是简单的调用了afterGettingFr
 *    ame1函数而已，只是因为C++中是不允许类成员函数作为回调函数的；ourHandlerClosure函数将在数据已
 *    经处理完毕时调用，如文件结束。
 */
void MultiFramedRTPSink::packFrame()
{
    // Get the next frame.
	//首先需要检查buffer中是否还存在溢出的数据（frame）
    // First, see if we have an overflow frame that was too big for the last pkt
    if (fOutBuf->haveOverflowData())
    {
        // Use this frame before reading a new one from the source
        unsigned frameSize = fOutBuf->overflowDataSize();
        struct timeval presentationTime = fOutBuf->overflowPresentationTime();
        unsigned durationInMicroseconds = fOutBuf->overflowDurationInMicroseconds();
        //使用溢出的数据作为packet的内容，注意这里并不一定进行memcpy操作
		//因为可能已经把packet的开始位置重置到overflow data 的位置
		fOutBuf->useOverflowData();
		
		//获取了数据，就可以准备发送了，当然若是数据量太小，将需要获取更多的数据
        afterGettingFrame1(frameSize, 0, presentationTime, durationInMicroseconds);
    }
    else
    {
        // Normal case: we need to read a new frame from the source
        if (fSource == NULL) return;
		
		//这里，给予当前帧预留空间的机会，保存一些特殊信息，当然frameSpecificHeaderSize函数返回0
        fCurFrameSpecificHeaderPosition = fOutBuf->curPacketSize();
        fCurFrameSpecificHeaderSize = frameSpecificHeaderSize();
        fOutBuf->skipBytes(fCurFrameSpecificHeaderSize);
        fTotalFrameSpecificHeaderSizes += fCurFrameSpecificHeaderSize;
		
		/*
		 *从source中获取数据，然后调用回调函数afterGettingFrame.注意，在C++中类成员函数是不能
		 *作为回调函数的。我们可以看到afterGettingFrame中直接调用了afterGettingFrame1函数，与
		 *上面的第一次情况处理类似了。不过这里为什么要回调函数回？
		 */
        fSource->getNextFrame(fOutBuf->curPtr(), fOutBuf->totalBytesAvailable(),
                              afterGettingFrame, this, ourHandleClosure, this);
    }
}

void MultiFramedRTPSink
::afterGettingFrame(void* clientData, unsigned numBytesRead,
                    unsigned numTruncatedBytes,
                    struct timeval presentationTime,
                    unsigned durationInMicroseconds)
{
    MultiFramedRTPSink* sink = (MultiFramedRTPSink*)clientData;
    sink->afterGettingFrame1(numBytesRead, numTruncatedBytes,
                             presentationTime, durationInMicroseconds);
}

/*
 *afterGettingFrame1的复杂之处在于处理frame的分片，若一个frame大于TCP/UDP有效载荷(程序中定义为1448个
 *字节),就必须分片了。最简单的情况就是一个packet(RTP包)中最多只允许一个frame，即一个RTP包中存在一个
 *frame或者frame的一个分片，H264就是这样处理的。方法是将剩余数据记录为buffer的溢出部分。下次调用packFrame
 *函数时，直接从溢出部分赋值到packet中。不过应该注意，一个frame的大小不能超过buffer的大小(默认为60000)，
 *否则会真的溢出，那就要考虑增加buffer的大小了。
 * 
 * 在packet中允许出现多个frame的情况下(大多数情况下应该每必要用到)，采用了第归来实现，可以看到afterGettingFrame1
 * 函数的最后有调用packFrame的代码
 */
void MultiFramedRTPSink
::afterGettingFrame1(unsigned frameSize, unsigned numTruncatedBytes,
                     struct timeval presentationTime,
                     unsigned durationInMicroseconds)
{
    if (fIsFirstPacket)
    {
		//第一个packet，则记录下当前时间
        // Record the fact that we're starting to play now:
        gettimeofday(&fNextSendTime, NULL);
    }

    fMostRecentPresentationTime = presentationTime;
    if (fInitialPresentationTime.tv_sec == 0 && fInitialPresentationTime.tv_usec == 0)
    {
        fInitialPresentationTime = presentationTime;
    }
	
	/*这里的处理要注意了，当一个Frame大于OutPacketBuffer::maxSize(默认为60000)时，
	 *则会丢弃剩下的部分，numTruncatedBytes即为超出部分的大小。
	 */
    if (numTruncatedBytes > 0)
    {
        unsigned const bufferSize = fOutBuf->totalBytesAvailable();
        envir() << "MultiFramedRTPSink::afterGettingFrame1(): The input frame data was too large for our buffer size ("
                << bufferSize << ").  "
                << numTruncatedBytes << " bytes of trailing data was dropped!  Correct this by increasing \"OutPacketBuffer::maxSize\" to at least "
                << OutPacketBuffer::maxSize + numTruncatedBytes << ", *before* creating this 'RTPSink'.  (Current value is "
                << OutPacketBuffer::maxSize << ".)\n";
    }
    unsigned curFragmentationOffset = fCurFragmentationOffset;
    unsigned numFrameBytesToUse = frameSize;
    unsigned overflowBytes = 0;

    // If we have already packed one or more frames into this packet,
    // check whether this new frame is eligible to be packed after them.
    // (This is independent of whether the packet has enough room for this
    // new frame; that check comes later.)
	//fNumFramesUsedSoFar>0表示packet已经存在frame，需要检查是否允许在packet中加入新的frame
    if (fNumFramesUsedSoFar > 0)
    {
        if ((fPreviousFrameEndedFragmentation
                && !allowOtherFramesAfterLastFragment())//不允许在前一个分片之后，跟随一个frame
                || !frameCanAppearAfterPacketStart(fOutBuf->curPtr(), frameSize))
        {//	不允许添加新的frame，则保存为溢出数据，下次处理
            // Save away this frame for next time:
            numFrameBytesToUse = 0;
            fOutBuf->setOverflowData(fOutBuf->curPacketSize(), frameSize,
                                     presentationTime, durationInMicroseconds);
        }
    }
    fPreviousFrameEndedFragmentation = False;

    if (numFrameBytesToUse > 0)
    {
        // Check whether this frame overflows the packet
        if (fOutBuf->wouldOverflow(frameSize))
        {
			/*
			 * 若frame将导致packet溢出，应该将其保存到packet的溢出数据中，在下一个packet中发送
			 * 如果frame本身大于packet的max size，就要对frame进行分片操作。不过需要调用allowFragmentationAfterStart
			 * 函数以确定是否允许分片，例如对H264而言
			 */
            // Don't use this frame now; instead, save it as overflow data, and
            // send it in the next packet instead.  However, if the frame is too
            // big to fit in a packet by itself, then we need to fragment it (and
            // use some of it in this packet, if the payload format permits this.)
            if (isTooBigForAPacket(frameSize)
                    && (fNumFramesUsedSoFar == 0 || allowFragmentationAfterStart()))
            {
                // We need to fragment this frame, and use some of it now:
                overflowBytes = computeOverflowForNewFrame(frameSize);
                numFrameBytesToUse -= overflowBytes;
                fCurFragmentationOffset += numFrameBytesToUse;
            }
            else
            {
                // We don't use any of this frame now:
                overflowBytes = frameSize;
                numFrameBytesToUse = 0;
            }
            fOutBuf->setOverflowData(fOutBuf->curPacketSize() + numFrameBytesToUse,
                                     overflowBytes, presentationTime, durationInMicroseconds);
        }
        else if (fCurFragmentationOffset > 0)
        {
            // This is the last fragment of a frame that was fragmented over
            // more than one packet.  Do any special handling for this case:
            fCurFragmentationOffset = 0;
            fPreviousFrameEndedFragmentation = True;
        }
    }

    if (numFrameBytesToUse == 0 && frameSize > 0)
    {
        // Send our packet now, because we have filled it up:
        sendPacketIfNecessary();	//发送RTP包
    }
    else
    {
        // Use this frame in our outgoing packet:
        unsigned char* frameStart = fOutBuf->curPtr();
        fOutBuf->increment(numFrameBytesToUse);
        // do this now, in case "doSpecialFrameHandling()" calls "setFramePadding()" to append padding bytes
		//还记得RTP头中预留的空间吗,将在这个函数中进行填充
        // Here's where any payload format specific processing gets done:
        doSpecialFrameHandling(curFragmentationOffset, frameStart,
                               numFrameBytesToUse, presentationTime,
                               overflowBytes);

        ++fNumFramesUsedSoFar;

		//设置下一个packet的时间信息，这里若存在overflow数据，就不需要更新时间，因为这是
		//同一个frame的不同分片，需要保证时间一致
        // Update the time at which the next packet should be sent, based
        // on the duration of the frame that we just packed into it.
        // However, if this frame has overflow data remaining, then don't
        // count its duration yet.
        if (overflowBytes == 0)
        {
            fNextSendTime.tv_usec += durationInMicroseconds;
            fNextSendTime.tv_sec += fNextSendTime.tv_usec/1000000;
            fNextSendTime.tv_usec %= 1000000;
        }

        // Send our packet now if (i) it's already at our preferred size, or
        // (ii) (heuristic) another frame of the same size as the one we just
        //      read would overflow the packet, or
        // (iii) it contains the last fragment of a fragmented frame, and we
        //      don't allow anything else to follow this or
        // (iv) one frame per packet is allowed:
        if (fOutBuf->isPreferredSize()
                || fOutBuf->wouldOverflow(numFrameBytesToUse)
                || (fPreviousFrameEndedFragmentation &&
                    !allowOtherFramesAfterLastFragment())
                || !frameCanAppearAfterPacketStart(fOutBuf->curPtr() - frameSize,
                        frameSize) )
        {
            // The packet is ready to be sent now
            sendPacketIfNecessary();	//发送RTP包
        }
        else
        {
            // There's room for more frames; try getting another:
            packFrame();	//packet中还可以容纳frame，这里将形成递归调用
        }
    }
}

static unsigned const rtpHeaderSize = 12;

Boolean MultiFramedRTPSink::isTooBigForAPacket(unsigned numBytes) const
{
    // Check whether a 'numBytes'-byte frame - together with a RTP header and
    // (possible) special headers - would be too big for an output packet:
    // (Later allow for RTP extension header!) #####
    numBytes += rtpHeaderSize + specialHeaderSize() + frameSpecificHeaderSize();
    return fOutBuf->isTooBigForAPacket(numBytes);
}

void MultiFramedRTPSink::sendPacketIfNecessary()
{
    if (fNumFramesUsedSoFar > 0)
    {
		//packet中存在frame，则发送出去
		// Send the packet:
		//可以通过宏TEST_LOSS宏，模拟10%丢包
#ifdef TEST_LOSS
        if ((our_random()%10) != 0) // simulate 10% packet loss #####
#endif
		//现在通过RTPInterface::sendPacket函数发送packet
		if (!fRTPInterface.sendPacket(fOutBuf->packet(), fOutBuf->curPacketSize()))
		{
			// if failure handler has been specified, call it
			if (fOnSendErrorFunc != NULL) (*fOnSendErrorFunc)(fOnSendErrorData);//错误处理
		}
        ++fPacketCount;
        fTotalOctetCount += fOutBuf->curPacketSize();
        fOctetCount += fOutBuf->curPacketSize()
                       - rtpHeaderSize - fSpecialHeaderSize - fTotalFrameSpecificHeaderSizes;

        ++fSeqNo; // for next time
    }

    if (fOutBuf->haveOverflowData()
            && fOutBuf->totalBytesAvailable() > fOutBuf->totalBufferSize()/2)
    {
		/*
		 * 为了提高效率，可以直接重置buffer中的socket开始位置，这样就不需要拷贝一遍overflow数据了。
		 * 在一个packet只能包含一个frame的情况下，是不是可以考虑修改这里的判断条件呢？
		 */
        // Efficiency hack: Reset the packet start pointer to just in front of
        // the overflow data (allowing for the RTP header and special headers),
        // so that we probably don't have to "memmove()" the overflow data
        // into place when building the next packet:
        unsigned newPacketStart = fOutBuf->curPacketSize()
                                  - (rtpHeaderSize + fSpecialHeaderSize + frameSpecificHeaderSize());
        fOutBuf->adjustPacketStart(newPacketStart);
    }
    else
    {
        // Normal case: Reset the packet start pointer back to the start:
        fOutBuf->resetPacketStart();//这种情况，若存在overflow data，就需要进行copy操作了
    }
    fOutBuf->resetOffset();	//packet已经发送了，可以重置buffer中的数据offset了
    fNumFramesUsedSoFar = 0;//清零packet中的frame数

	/*
	 * 数据已经发送完毕(例如文件传输完毕)，就可以关闭了
	 */
    if (fNoFramesLeft)
    {
        // We're done:
        onSourceClosure(this);
    }
    else
    {
		/*
		 * 准备下一次发送任务
		 */
        // We have more frames left to send.  Figure out when the next frame
        // is due to start playing, then make sure that we wait this long before
        // sending the next packet.
        struct timeval timeNow;
        gettimeofday(&timeNow, NULL);
        int secsDiff = fNextSendTime.tv_sec - timeNow.tv_sec;
        int64_t uSecondsToGo = secsDiff*1000000 + (fNextSendTime.tv_usec - timeNow.tv_usec);
        if (uSecondsToGo < 0 || secsDiff < 0)   // sanity check: Make sure that the time-to-delay is non-negative:
        {
            uSecondsToGo = 0;
        }
		
		//作演示时间，处理函数，将入到任务调度器中，以便进行下一次发送操作
        // Delay this amount of time:
        nextTask() = envir().taskScheduler().scheduleDelayedTask(uSecondsToGo, (TaskFunc*)sendNext, this);
    }
}

// The following is called after each delay between packet sends:
void MultiFramedRTPSink::sendNext(void* firstArg)
{
    MultiFramedRTPSink* sink = (MultiFramedRTPSink*)firstArg;
    sink->buildAndSendPacket(False);
}

void MultiFramedRTPSink::ourHandleClosure(void* clientData)
{
    MultiFramedRTPSink* sink = (MultiFramedRTPSink*)clientData;
    // There are no frames left, but we may have a partially built packet
    //  to send
    sink->fNoFramesLeft = True;
    sink->sendPacketIfNecessary();
}
