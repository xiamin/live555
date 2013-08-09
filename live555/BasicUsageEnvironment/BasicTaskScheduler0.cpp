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

#include "BasicUsageEnvironment0.hh"
#include "HandlerSet.hh"

////////// A subclass of DelayQueueEntry,
//////////     used to implement BasicTaskScheduler0::scheduleDelayedTask()

class AlarmHandler: public DelayQueueEntry {
public:
	AlarmHandler(TaskFunc* proc, void* clientData, DelayInterval timeToDelay) :
		DelayQueueEntry(timeToDelay), fProc(proc), fClientData(clientData) {
	}

private:
	// redefined virtual functions
	virtual void handleTimeout() {
		(*fProc)(fClientData);
		DelayQueueEntry::handleTimeout();
	}

private:
	TaskFunc* fProc;
	void* fClientData;
};

////////// BasicTaskScheduler0 //////////

/*
 * BasicTaskScheduler0实现了延迟任务和触发事件。触发事件是通过一个机器字长所能表示的位来处理的，在内部
 * 被定义为fTriggersAwaitingHandling，是int类型。从最高位开始保存。比如，假设系统是32位机，并且只有
 * 一个待触发事件，那么fTriggerAwaitingHandling的值为0x80000000.该类中还保存了上一次触发事件的ID以
 * 及mask，作为下一个调度的起点。这样保证了调度程序能够有序的执行所有待触发事件，而不是每次都从头开始。
 */

/*
 * 类BasicTaskScheduler0的构造函数，进行一系列初始化工作
 */
BasicTaskScheduler0::BasicTaskScheduler0() :
	fLastHandledSocketNum(-1), fTriggersAwaitingHandling(0),
			fLastUsedTriggerMask(1), fLastUsedTriggerNum(MAX_NUM_EVENT_TRIGGERS
					- 1) {
	fHandlers = new HandlerSet;
	for (unsigned i = 0; i < MAX_NUM_EVENT_TRIGGERS; ++i) {
		fTriggeredEventHandlers[i] = NULL;
		fTriggeredEventClientDatas[i] = NULL;
	}
}
/*
 * 析构函数
 */
BasicTaskScheduler0::~BasicTaskScheduler0() {
	delete fHandlers;
}

TaskToken BasicTaskScheduler0::scheduleDelayedTask(int64_t microseconds,
		TaskFunc* proc, void* clientData) {
	if (microseconds < 0)
		microseconds = 0;
	//DelayInterval是表示时间差的结构
	DelayInterval timeToDelay((long) (microseconds / 1000000),
			(long) (microseconds % 1000000));
	//创建delayQueue中的一项
	AlarmHandler* alarmHandler =
			new AlarmHandler(proc, clientData, timeToDelay);
	//加入Delayqueue
	fDelayQueue.addEntry(alarmHandler);
	//返回delay task的唯一标志
	return (void*) (alarmHandler->token());
}

void BasicTaskScheduler0::unscheduleDelayedTask(TaskToken& prevTask) {
	DelayQueueEntry* alarmHandler =
			fDelayQueue.removeEntry((intptr_t) prevTask);
	prevTask = NULL;
	delete alarmHandler;
}

void BasicTaskScheduler0::doEventLoop(char* watchVariable) {
	// Repeatedly loop, handling readble sockets and timed events:
	while (1) {
		if (watchVariable != NULL && *watchVariable != 0)
			break;
		SingleStep();
	}
}

EventTriggerId BasicTaskScheduler0::createEventTrigger(
		TaskFunc* eventHandlerProc) {
	unsigned i = fLastUsedTriggerNum;	//unsigned = unsigned int 32位，4个字节
	EventTriggerId mask = fLastUsedTriggerMask;
	/*
	 * 在数组中寻找一个未使用的 ,把eventHandlerProc分配到这一项
	 */
	do {
		i = (i + 1) % MAX_NUM_EVENT_TRIGGERS;
		mask >>= 1;
		if (mask == 0)
			mask = 0x80000000;

		if (fTriggeredEventHandlers[i] == NULL) {
			// This trigger number is free; use it:
			fTriggeredEventHandlers[i] = eventHandlerProc;
			fTriggeredEventClientDatas[i] = NULL; // sanity

			fLastUsedTriggerMask = mask;
			fLastUsedTriggerNum = i;

			return mask; //分配成功，返回值表明了第几项
		}
	} while (i != fLastUsedTriggerNum);//表明了在数组中循环一圈

	//数组中的所有项都被占用，返回表明失败
	// All available event triggers are allocated; return 0 instead:
	return 0;
}

void BasicTaskScheduler0::deleteEventTrigger(EventTriggerId eventTriggerId) {
	fTriggersAwaitingHandling &= ~eventTriggerId;

	if (eventTriggerId == fLastUsedTriggerMask) { // common-case optimization:
		fTriggeredEventHandlers[fLastUsedTriggerNum] = NULL;
		fTriggeredEventClientDatas[fLastUsedTriggerNum] = NULL;
	} else {
		// "eventTriggerId" should have just one bit set.
		// However, we do the reasonable thing if the user happened to 'or' together two or more "EventTriggerId"s:
		EventTriggerId mask = 0x80000000;
		for (unsigned i = 0; i < MAX_NUM_EVENT_TRIGGERS; ++i) {
			if ((eventTriggerId & mask) != 0) {
				fTriggeredEventHandlers[i] = NULL;
				fTriggeredEventClientDatas[i] = NULL;
			}
			mask >>= 1;
		}
	}
}

void BasicTaskScheduler0::triggerEvent(EventTriggerId eventTriggerId,
		void* clientData) {
	// First, record the "clientData".  (Note that we allow "eventTriggerId" to be a combination of bits for multiple events.)
	EventTriggerId mask = 0x80000000;
	/*
	 * 从头到尾查找eventTriggerId对应的项，保存下clientData
	 */
	for (unsigned i = 0; i < MAX_NUM_EVENT_TRIGGERS; ++i) {
		if ((eventTriggerId & mask) != 0) {
			fTriggeredEventClientDatas[i] = clientData;
		}
		mask >>= 1;
	}

	// Then, note this event as being ready to be handled.
	// (Note that because this function (unlike others in the library) can be called from an external thread, we do this last, to
	//  reduce the risk of a race condition.)
	fTriggersAwaitingHandling |= eventTriggerId;
}

////////// HandlerSet (etc.) implementation //////////

HandlerDescriptor::HandlerDescriptor(HandlerDescriptor* nextHandler) :
	conditionSet(0), handlerProc(NULL) {
	// Link this descriptor into a doubly-linked list:
	if (nextHandler == this) { // initialization
		fNextHandler = fPrevHandler = this;
	} else {
		fNextHandler = nextHandler;
		fPrevHandler = nextHandler->fPrevHandler;
		nextHandler->fPrevHandler = this;
		fPrevHandler->fNextHandler = this;
	}
}
/*
 * 双向链表的节点
 */
HandlerDescriptor::~HandlerDescriptor() {
	// Unlink this descriptor from a doubly-linked list:
	fNextHandler->fPrevHandler = fPrevHandler;
	fPrevHandler->fNextHandler = fNextHandler;
}

HandlerSet::HandlerSet() :
	fHandlers(&fHandlers) {
	fHandlers.socketNum = -1; // shouldn't ever get looked at, but in case...
}

HandlerSet::~HandlerSet() {
	// Delete each handler descriptor:
	while (fHandlers.fNextHandler != &fHandlers) {
		delete fHandlers.fNextHandler; // changes fHandlers->fNextHandler
	}
}

/*
 * socket handler保存在队列BasicTaskScheduler0::HandlerSet* fHandler中
 * event handler保存在数组BasicTaskScheduler0::TaskFunc* fTriggeredEventHandler
 * [MAX_NUM_EVENT_TRIGGERS]中
 * delay task保存在队列BasicTaskScheduler0::DelayQueue fDelayQueue中
 *
 * socket handler为typedef void BackgroundHandlerProc(void* clientData, int mask);
 * event handler为typedef void TaskFunc(void* clientData);
 * delay task 为typedef void TaskFunc(void* clientData);//跟event handler一样
 *
 * 再看下向任务调度对象添加三种任务的函数的样子
 * delay task为：void setBackgroundHandling(int socketNum, int conditionSet　,
 * 						BackgroundHandlerProc* handlerProc, void* clientData)
 * event handler为：EventTriggerId createEventTrigger(TaskFunc* eventHandlerProc)
 * delay task为：TaskToken scheduleDelayedTask(int64_t microseconds,
 * 												TaskFunc* proc,void* clientData)
 */

/*
 * 下面handlerSet类成员的定义实现了双向链表的增删查改
 */
/*
 * socket handler添加时为什么需要那些参数呢？socketnum是需要的，因为要select socket
 * （socketNum即是socket(返回的那个socket对象)。conditionSet也是需要的，它用于表明socket
 * 在select时查看哪种状态，是可读，可写还是出错？proc和clientData着两个参数就不必说了
 *
 * BackgroudHandlerProc的参数，socketNum不必解释，mask是什么呢？它正是对应着conditionSet，
 * 但它表明的是select之后的结果，不如一个socket可能需要检查其读/写状态，而当前只能读，不能写，那么
 * mask中就只有表明读的位被设置
 *
 * event handler是被存在数组中。数组大小固定，是32项，用EventTriggerId来表示数组中的项，
 * EventTriggerId是一个32位整数，因为数组是32项，所以用EventTriggerId中的第n位置1表明对应数组
 * 中的第n项。成员变量fTriggerAwaitingHandling也是EventTriggerId类型，它里面置1的那些位对应了
 * 数组中所有需要处理的项。这样做节省了内存和计算，但降低了代码的可读性，而且不是很灵活，只能支持32项
 * 或64项，其他数量不被支持
 */
void HandlerSet::assignHandler(int socketNum, int conditionSet,
		TaskScheduler::BackgroundHandlerProc* handlerProc, void* clientData) {
	// First, see if there's already a handler for this socket:
	HandlerDescriptor* handler = lookupHandler(socketNum);
	if (handler == NULL) { // No existing handler, so create a new descr:
		handler = new HandlerDescriptor(fHandlers.fNextHandler);
		handler->socketNum = socketNum;
	}

	handler->conditionSet = conditionSet;
	handler->handlerProc = handlerProc;
	handler->clientData = clientData;
}

void HandlerSet::clearHandler(int socketNum) {
	HandlerDescriptor* handler = lookupHandler(socketNum);
	delete handler;
}

void HandlerSet::moveHandler(int oldSocketNum, int newSocketNum) {
	HandlerDescriptor* handler = lookupHandler(oldSocketNum);
	if (handler != NULL) {
		handler->socketNum = newSocketNum;
	}
}

HandlerDescriptor* HandlerSet::lookupHandler(int socketNum) {
	HandlerDescriptor* handler;
	HandlerIterator iter(*this);
	while ((handler = iter.next()) != NULL) {
		if (handler->socketNum == socketNum)
			break;
	}
	return handler;
}

HandlerIterator::HandlerIterator(HandlerSet& handlerSet) :
	fOurSet(handlerSet) {
	reset();
}

HandlerIterator::~HandlerIterator() {
}

void HandlerIterator::reset() {
	fNextPtr = fOurSet.fHandlers.fNextHandler;
}

HandlerDescriptor* HandlerIterator::next() {
	HandlerDescriptor* result = fNextPtr;
	if (result == &fOurSet.fHandlers) { // no more
		result = NULL;
	} else {
		fNextPtr = fNextPtr->fNextHandler;
	}

	return result;
}
