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
// Copyright (c) 1996-2013 Live Networks, Inc.  All rights reserved.
// Basic Usage Environment: for a simple, non-scripted, console application
// Implementation


#include "BasicUsageEnvironment.hh"
#include "HandlerSet.hh"
#include <stdio.h>
#if defined(_QNX4)
#include <sys/select.h>
#include <unix.h>
#endif

////////// BasicTaskScheduler //////////

BasicTaskScheduler* BasicTaskScheduler::createNew(
		unsigned maxSchedulerGranularity) {
	return new BasicTaskScheduler(maxSchedulerGranularity);
}

BasicTaskScheduler::BasicTaskScheduler(unsigned maxSchedulerGranularity) :
	fMaxSchedulerGranularity(maxSchedulerGranularity), fMaxNumSockets(0) {
	FD_ZERO(&fReadSet);
	FD_ZERO(&fWriteSet);
	FD_ZERO(&fExceptionSet);

	if (maxSchedulerGranularity > 0)
		schedulerTickTask(); // ensures that we handle events frequently
}

BasicTaskScheduler::~BasicTaskScheduler() {
}

void BasicTaskScheduler::schedulerTickTask(void* clientData) {
	((BasicTaskScheduler*) clientData)->schedulerTickTask();
}

void BasicTaskScheduler::schedulerTickTask() {
	scheduleDelayedTask(fMaxSchedulerGranularity, schedulerTickTask, this);
}

#ifndef MILLION
#define MILLION 1000000
#endif
/*
 * 1.首先处理IO事件。程序通过select函数选择那些已经准备好的IO文件描述符，对其进行读、写或者异常
 * 	 处理。与该任务相关的接口为：SetBackgroundHandling, disableBackgroundHandling以及
 *   moveSocketHandling。IO事件会在任务中反复执行
 * 2.然后处理触发事件。由于作者采用了一个机器字节中的位来保存出发事件的数量，所以触发事件的数量
 *   受机器限制。对X86系统则32个触发事件。这样做的好处是效率高，但缺点是数量受限制。与该任务相关
 *   的接口为createEventTrigger,deleteEventTrigger和triggerEvent。事件一旦被触发后，
 *   将会立即删除，避免事件再次触发。
 * 3.最后将执行延迟任务。延迟任务保存在一个延迟队列中，通过时间来指定何时执行。有关延迟任务，我们
 *   放在后面的小节中专门学习。延迟任务执行后也将从延迟队列中删除。
 */
/*
 * 1.为所有需要操作的socket执行select
 * 2.找出第一个应执行socket任务（handler）并执行之
 * 3.找到第一个应响应的事件，并执行之
 * 4.找到第一个应执行的延迟任务并执行之
 */
void BasicTaskScheduler::SingleStep(unsigned maxDelayTime) {
	fd_set readSet = fReadSet; // make a copy for this select() call
	fd_set writeSet = fWriteSet; // ditto
	fd_set exceptionSet = fExceptionSet; // ditto

	DelayInterval const& timeToDelay = fDelayQueue.timeToNextAlarm();
	struct timeval tv_timeToDelay;
	tv_timeToDelay.tv_sec = timeToDelay.seconds();
	tv_timeToDelay.tv_usec = timeToDelay.useconds();

	// Very large "tv_sec" values cause select() to fail.
	// Don't make it any larger than 1 million seconds (11.5 days)
	/*
	 * 一个太大的tv_set将导致select函数失败
	 * 因此，确保它不大于一万秒，即11.5天
	 */
	const long MAX_TV_SEC = MILLION;
	if (tv_timeToDelay.tv_sec > MAX_TV_SEC) {
		tv_timeToDelay.tv_sec = MAX_TV_SEC;
	}

	// Also check our "maxDelayTime" parameter (if it's > 0):
	/*
	 * 检查最大延迟时间是否大于一万秒，以及微妙级是否大于一百万微妙，即1秒
	 */
	if (maxDelayTime > 0 && (tv_timeToDelay.tv_sec > (long) maxDelayTime
			/ MILLION
			|| (tv_timeToDelay.tv_sec == (long) maxDelayTime / MILLION
					&& tv_timeToDelay.tv_usec > (long) maxDelayTime % MILLION))) {
		tv_timeToDelay.tv_sec = maxDelayTime / MILLION;
		tv_timeToDelay.tv_usec = maxDelayTime % MILLION;
	}

	int selectResult = select(fMaxNumSockets, &readSet, &writeSet,
			&exceptionSet, &tv_timeToDelay);
	if (selectResult < 0) {
#if defined(__WIN32__) || defined(_WIN32)
		int err = WSAGetLastError();
		// For some unknown reason, select() in Windoze sometimes fails with WSAEINVAL if
		// it was called with no entries set in "readSet".  If this happens, ignore it:
		if (err == WSAEINVAL && readSet.fd_count == 0) {
			err = EINTR;
			// To stop this from happening again, create a dummy socket:
			int dummySocketNum = socket(AF_INET, SOCK_DGRAM, 0);
			FD_SET((unsigned)dummySocketNum, &fReadSet);
		}
		if (err != EINTR) {
#else
		if (errno != EINTR && errno != EAGAIN) {
#endif
			// Unexpected error - treat this as fatal:
#if !defined(_WIN32_WCE)
			perror("BasicTaskScheduler::SingleStep(): select() fails");
			// Because this failure is often "Bad file descriptor" - which is caused by an invalid socket number (i.e., a socket number
			// that had already been closed) being used in "select()" - we print out the sockets that were being used in "select()",
			// to assist in debugging:
			fprintf(stderr, "socket numbers used in the select() call:");
			for (int i = 0; i < 10000; ++i) {
				if (FD_ISSET(i, &fReadSet) || FD_ISSET(i, &fWriteSet)
						|| FD_ISSET(i, &fExceptionSet)) {
					fprintf(stderr, " %d(", i);
					if (FD_ISSET(i, &fReadSet))
						fprintf(stderr, "r");
					if (FD_ISSET(i, &fWriteSet))
						fprintf(stderr, "w");
					if (FD_ISSET(i, &fExceptionSet))
						fprintf(stderr, "e");
					fprintf(stderr, ")");
				}
			}
			fprintf(stderr, "\n");
#endif
			internalError();
		}
	}

	/*
	 * BasicTaskScheduler实现了最后一个任务，即IO事件和核心调度程序SingleStep。IO任务的实现也很简单，它被
	 * 定义为一个双向循环链表，链表的节点为HandlerDiscriptor.其实现类为HandlerSet，该类实现了对链表的增删改
	 * 查操作。与之对应的，作者还定义了一个迭代器类HandlerIterator，用于遍历链表。而调度程序则实现上面所提到的
	 * 三步操作，来依次执行各类任务。现在给出该函数的实现
	 */
	// Call the handler function for one readable socket:
	/*
	 * 迭代器
	 */

	HandlerIterator iter(*fHandlers);
	HandlerDescriptor* handler;

	// To ensure forward progress through the handlers, begin past the last
	// socket number that we handled:
	/*
	 * 找到上次执行后的处理程序队列中的下一个
	 * 这里先找到上次执行时的socket号
	 */
	if (fLastHandledSocketNum >= 0) {
		while ((handler = iter.next()) != NULL) {
			if (handler->socketNum == fLastHandledSocketNum)
				break;
		}
		/*
		 * 没有找到，可能已经被移除，重置延时队列
		 */
		if (handler == NULL) {
			fLastHandledSocketNum = -1;
			iter.reset(); // start from the beginning instead
		}
	}

	/*
	 * 从找到的handler开始，执行其下一个，不管其状态是什么，皆执行
	 * 当然，也可能从队列头开始执行
	 */
	/*
	 * 在调用select函数后，用FD_ISSET来检测fd在fdset集合中的状态是否变化
	 * 返回整形，当检测到fd状态发生变化时返回真（非0），否则，返回假（0）
	 */
	while ((handler = iter.next()) != NULL) {
		int sock = handler->socketNum; // alias
		int resultConditionSet = 0;

		/*
		 * 检查
		 */
		if (FD_ISSET(sock, &readSet) && FD_ISSET(sock, &fReadSet)/*sanity check*/)
			resultConditionSet |= SOCKET_READABLE;
		if (FD_ISSET(sock, &writeSet) && FD_ISSET(sock, &fWriteSet)/*sanity check*/)
			resultConditionSet |= SOCKET_WRITABLE;
		if (FD_ISSET(sock, &exceptionSet) && FD_ISSET(sock, &fExceptionSet)/*sanity check*/)
			resultConditionSet |= SOCKET_EXCEPTION;

		if ((resultConditionSet & handler->conditionSet) != 0
				&& handler->handlerProc != NULL) {

			/*
			 * 保存sock号，调度程序下次将从该位置继续执行，下同
			 */
			fLastHandledSocketNum = sock;
			// Note: we set "fLastHandledSocketNum" before calling the handler,
			// in case the handler calls "doEventLoop()" reentrantly.
			/*
			 * 调用事件处理函数
			 */
			(*handler->handlerProc)(handler->clientData, resultConditionSet);
			break;
		}
	}

	/*
	 * 我们没有调用处理程序，因此重再来一次
	 * 造成这样的原因可能是从上一次执行处理程序的位置向后没有找到任何可执行的处理程序了
	 * 于是从头开始寻找处理程序
	 */
	if (handler == NULL && fLastHandledSocketNum >= 0) {
		// We didn't call a handler, but we didn't get to check all of them,
		// so try again from the beginning:
		iter.reset();
		while ((handler = iter.next()) != NULL) {
			int sock = handler->socketNum; // alias
			int resultConditionSet = 0;
			if (FD_ISSET(sock, &readSet) && FD_ISSET(sock, &fReadSet)/*sanity check*/)
				resultConditionSet |= SOCKET_READABLE;
			if (FD_ISSET(sock, &writeSet) && FD_ISSET(sock, &fWriteSet)/*sanity check*/)
				resultConditionSet |= SOCKET_WRITABLE;
			if (FD_ISSET(sock, &exceptionSet) && FD_ISSET(sock, &fExceptionSet)/*sanity check*/)
				resultConditionSet |= SOCKET_EXCEPTION;
			if ((resultConditionSet & handler->conditionSet) != 0
					&& handler->handlerProc != NULL) {
				fLastHandledSocketNum = sock;
				// Note: we set "fLastHandledSocketNum" before calling the handler,
				// in case the handler calls "doEventLoop()" reentrantly.
				(*handler->handlerProc)(handler->clientData, resultConditionSet);
				break;
			}
		}
		/*
		 * 调用事件处理函数
		 */
		/*
		 * 依然没有找到任何可执行的handler
		 * 将其值置为-1
		 * 以告诉处理程序，下次应该从头开始找handler
		 */
		if (handler == NULL)
			fLastHandledSocketNum = -1;//because we didn't call a handler
	}

	/*
	 * 响应新触发的事件
	 */
	// Also handle any newly-triggered event (Note that we do this *after* calling a socket handler,
	// in case the triggered event handler modifies The set of readable sockets.)
	if (fTriggersAwaitingHandling != 0) {

		/*
		 * 首先检查是否只有一个待触发事件
		 */
		if (fTriggersAwaitingHandling == fLastUsedTriggerMask) {
			// Common-case optimization for a single event trigger:
			fTriggersAwaitingHandling = 0;
			if (fTriggeredEventHandlers[fLastUsedTriggerNum] != NULL) {
				/*
				 * 执行事件处理函数
				 */
				(*fTriggeredEventHandlers[fLastUsedTriggerNum])(
						fTriggeredEventClientDatas[fLastUsedTriggerNum]);
			}
		} else {
			/*
			 * 寻找待执行的触发事件
			 */
			// Look for an event trigger that needs handling (making sure that we make forward progress through all possible triggers):
			unsigned i = fLastUsedTriggerNum;
			EventTriggerId mask = fLastUsedTriggerMask;

			do {
				i = (i + 1) % MAX_NUM_EVENT_TRIGGERS;
				mask >>= 1;
				if (mask == 0)
					mask = 0x80000000;

				if ((fTriggersAwaitingHandling & mask) != 0) {
					fTriggersAwaitingHandling &= ~mask;
					if (fTriggeredEventHandlers[i] != NULL) {
						/*
						 * 响应事件
						 */
						(*fTriggeredEventHandlers[i])(
								fTriggeredEventClientDatas[i]);
					}

					fLastUsedTriggerMask = mask;
					fLastUsedTriggerNum = i;
					break;
				}
			} while (i != fLastUsedTriggerNum);
		}
	}
	/*
	 * 最后执行一个最迫切的任务
	 */
	// Also handle any delayed event that may have come due.
	fDelayQueue.handleAlarm();
}

void BasicTaskScheduler::setBackgroundHandling(int socketNum, int conditionSet,
		BackgroundHandlerProc* handlerProc, void* clientData) {
	if (socketNum < 0)
		return;
	FD_CLR((unsigned)socketNum, &fReadSet);
	FD_CLR((unsigned)socketNum, &fWriteSet);
	FD_CLR((unsigned)socketNum, &fExceptionSet);
	if (conditionSet == 0) {
		fHandlers->clearHandler(socketNum);
		if (socketNum + 1 == fMaxNumSockets) {
			--fMaxNumSockets;
		}
	} else {
		fHandlers->assignHandler(socketNum, conditionSet, handlerProc,
				clientData);
		if (socketNum + 1 > fMaxNumSockets) {
			fMaxNumSockets = socketNum + 1;
		}
		if (conditionSet & SOCKET_READABLE)
			FD_SET((unsigned)socketNum, &fReadSet);
		if (conditionSet & SOCKET_WRITABLE)
			FD_SET((unsigned)socketNum, &fWriteSet);
		if (conditionSet & SOCKET_EXCEPTION)
			FD_SET((unsigned)socketNum, &fExceptionSet);
	}
}

void BasicTaskScheduler::moveSocketHandling(int oldSocketNum, int newSocketNum) {
	if (oldSocketNum < 0 || newSocketNum < 0)
		return; // sanity check
	if (FD_ISSET(oldSocketNum, &fReadSet)) {
		FD_CLR((unsigned)oldSocketNum, &fReadSet);
		FD_SET((unsigned)newSocketNum, &fReadSet);
	}
	if (FD_ISSET(oldSocketNum, &fWriteSet)) {
		FD_CLR((unsigned)oldSocketNum, &fWriteSet);
		FD_SET((unsigned)newSocketNum, &fWriteSet);
	}
	if (FD_ISSET(oldSocketNum, &fExceptionSet)) {
		FD_CLR((unsigned)oldSocketNum, &fExceptionSet);
		FD_SET((unsigned)newSocketNum, &fExceptionSet);
	}
	fHandlers->moveHandler(oldSocketNum, newSocketNum);

	if (oldSocketNum + 1 == fMaxNumSockets) {
		--fMaxNumSockets;
	}
	if (newSocketNum + 1 > fMaxNumSockets) {
		fMaxNumSockets = newSocketNum + 1;
	}
}
