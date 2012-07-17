/*
 * WIN32 Events for POSIX
 * Author: Mahmoud Al-Qudsi <mqudsi@neosmart.net>
 * Copyright (C) 2011 - 2012 by NeoSmart Technologies
 * This code is released under the terms of the MIT License
 */

#include "pevents.h"
#include <assert.h>
#include <errno.h>
#include <sys/time.h>
#ifdef WFMO
#include <deque>
#endif

namespace neosmart
{
#ifdef WFMO
	struct neosmart_wfmo_t_
	{
		pthread_mutex_t Mutex;
		pthread_cond_t CVariable;
		union
		{
			int FiredEvent; //WFSO
			int EventsLeft; //WFMO
		} Status;
		bool StillWaiting;
		int RefCount;
		bool WaitAll;
		
		void Destroy()
		{
			pthread_mutex_destroy(&Mutex);
			pthread_cond_destroy(&CVariable);
		}
	};
	typedef neosmart_wfmo_t_ *neosmart_wfmo_t;
	
	struct neosmart_wfmo_info_t_
	{
		neosmart_wfmo_t Waiter;
		int WaitIndex;
	};
	typedef neosmart_wfmo_info_t_ *neosmart_wfmo_info_t;
#endif
	
	struct neosmart_event_t_
	{
		bool AutoReset;
		pthread_cond_t CVariable;
		pthread_mutex_t Mutex;
		bool State;
#ifdef WFMO
		std::deque<neosmart_wfmo_info_t_> RegisteredWaits;
#endif
	};
	
	neosmart_event_t CreateEvent(bool manualReset, bool initialState)
	{
		neosmart_event_t event = new neosmart_event_t_;
		
		int result = pthread_cond_init(&event->CVariable, 0);
#ifdef PTHREADCHK
		if(result != 0)
			return NULL;
#endif
		
		result = pthread_mutex_init(&event->Mutex, 0);
#ifdef PTHREADCHK
		if(result != 0)
			return NULL;
#endif
		
		event->State = false;
		event->AutoReset = !manualReset;
		
		result = SetEvent(event);
#ifdef PTHREADCHK
		if(initialState && (result != 0))
		{
			assert(false);
			return NULL; //Shouldn't ever happen
		}
#endif
		
		return event;
	}
	
	int UnlockedWaitForEvent(neosmart_event_t event, uint64_t milliseconds)
	{
		int result = 0;
		if(!event->State)
		{	
			//Zero-timeout event state check optimization
			if(milliseconds == 0)
			{
				return ETIMEDOUT;
			}
			
			timespec ts;
			if(milliseconds != (uint64_t) -1)
			{
				timeval tv;
				gettimeofday(&tv, NULL);
				
				uint64_t nanoseconds = ((uint64_t) tv.tv_sec) * 1000 * 1000 * 1000 + milliseconds * 1000 * 1000 + ((uint64_t) tv.tv_usec) * 1000;
				
				ts.tv_sec = nanoseconds / 1000 / 1000 / 1000;
				ts.tv_nsec = (nanoseconds - ((uint64_t) ts.tv_sec) * 1000 * 1000 * 1000);
			}
			
			do
			{
				//Regardless of whether it's an auto-reset or manual-reset event:
				//wait to obtain the event, then lock anyone else out
				if(milliseconds != (uint64_t) -1)
				{
					result = pthread_cond_timedwait(&event->CVariable, &event->Mutex, &ts);
				}
				else
				{
					result = pthread_cond_wait(&event->CVariable, &event->Mutex);
				}
			} while(result == 0 && !event->State);
			
			if(result == 0 && event->AutoReset)
			{
				//We've only accquired the event if the wait succeeded
				event->State = false;
			}
		}
		else if(event->AutoReset)
		{
			//It's an auto-reset event that's currently available;
			//we need to stop anyone else from using it
			result = 0;
			event->State = false;
		}
		//Else we're trying to obtain a manual reset event with a signalled state;
		//don't do anything
		
		return result;
	}
	
	int WaitForEvent(neosmart_event_t event, uint64_t milliseconds)
	{
		int result = pthread_mutex_lock(&event->Mutex);
#ifdef PTHREADCHK
		if(result != 0)
		{
			return result;
		}
#endif
        
		result = UnlockedWaitForEvent(event, milliseconds);
		
		int tempResult = pthread_mutex_unlock(&event->Mutex);
#ifdef PTHREADCHK
		if (tempResult != 0)
		{
			//Inconsistent state. Cannot continue after this.
			assert(false);
			return tempResult;
		}
#endif
		
		return result;
	}
	
#ifdef WFMO
	int WaitForMultipleEvents(neosmart_event_t *events, int count, bool waitAll, uint64_t milliseconds)
	{
		int unused;
		return WaitForMultipleEvents(events, count, waitAll, milliseconds, unused);
	}
	
	int WaitForMultipleEvents(neosmart_event_t *events, int count, bool waitAll, uint64_t milliseconds, int &waitIndex)
	{
		neosmart_wfmo_t wfmo = new neosmart_wfmo_t_;
		
		int result = pthread_mutex_init(&wfmo->Mutex, 0);
#ifdef PTHREADCHK
		if(result != 0)
		{
			delete wfmo;
			return result;
		}
#endif
		
		result = pthread_cond_init(&wfmo->CVariable, 0);
#ifdef PTHREADCHK
		if(result != 0)
		{
			pthread_mutex_destroy(&wfmo->Mutex);
			delete wfmo;
			return result;
		}
#endif
		
		neosmart_wfmo_info_t_ waitInfo;
		waitInfo.Waiter = wfmo;
		waitInfo.WaitIndex = -1;
		
		wfmo->WaitAll = waitAll;
		wfmo->StillWaiting = true;
		wfmo->RefCount = 1;
		
		if(waitAll)
		{
			wfmo->Status.EventsLeft = count;
		}
		else
		{
			wfmo->Status.FiredEvent = -1;
		}
		
		result = pthread_mutex_lock(&wfmo->Mutex);
#ifdef PTHREADCHK
		if (result != 0)
		{
			wfmo->Destroy();
			delete wfmo;
			return result;
		}
#endif
		
		bool done = false;
		waitIndex = -1;
		
		for(int i = 0; i < count; ++i)
		{
			waitInfo.WaitIndex = i;
			
			//Must not release lock until RegisteredWait is potentially added
			result = pthread_mutex_lock(&events[i]->Mutex);
#ifdef PTHREADCHK
			if(result != 0)
			{
				//Inconsistent state. Cannot continue after this.
				assert(false);
				return result;
			}
#endif
			
			if(UnlockedWaitForEvent(events[i], 0) == 0)
			{
				result = pthread_mutex_unlock(&events[i]->Mutex);
#ifdef PTHREADCHK
				if(result != 0)
				{
					//Inconsistent state. Cannot continue after this.
					assert(false);
					return result;
				}
#endif
				
				if(waitAll)
				{
					--wfmo->Status.EventsLeft;
					assert(wfmo->Status.EventsLeft >= 0);
				}
				else
				{
					wfmo->Status.FiredEvent = i;
					waitIndex = i;
					done = true;
					break;
				}
			}
			else
			{				
				events[i]->RegisteredWaits.push_back(waitInfo);
				++wfmo->RefCount;
				
				int tempResult = pthread_mutex_unlock(&events[i]->Mutex);
#ifdef PTHREADCHK
				if(tempResult != 0)
				{
					//Inconsistent state. Cannot continue after this.
					assert(false);
					return tempResult;
				}
#endif
			}
		}
		
		timespec ts;
		if(!done)
		{
			if(milliseconds == 0)
			{
				result = ETIMEDOUT;
				done = true;
			}
			else if(milliseconds != (uint64_t) -1)
			{
				timeval tv;
				gettimeofday(&tv, NULL);
				
				uint64_t nanoseconds = ((uint64_t) tv.tv_sec) * 1000 * 1000 * 1000 + milliseconds * 1000 * 1000 + ((uint64_t) tv.tv_usec) * 1000;
				
				ts.tv_sec = nanoseconds / 1000 / 1000 / 1000;
				ts.tv_nsec = (nanoseconds - ((uint64_t) ts.tv_sec) * 1000 * 1000 * 1000);
			}
		}
		
		while(!done)
		{
			//One (or more) of the events we're monitoring has been triggered?
			
			//If we're waiting for all events, assume we're done and check if there's an event that hasn't fired
			//But if we're waiting for just one event, assume we're not done until we find a fired event
			done = (waitAll && wfmo->Status.EventsLeft == 0) || (!waitAll && wfmo->Status.FiredEvent != -1);
			
			if(!done)
			{
				if(milliseconds != (uint64_t) -1)
				{
					result = pthread_cond_timedwait(&wfmo->CVariable, &wfmo->Mutex, &ts);
				}
				else
				{
					result = pthread_cond_wait(&wfmo->CVariable, &wfmo->Mutex);
				}
				
				if(result != 0)
					break;
			}
		}
		
		waitIndex = wfmo->Status.FiredEvent;
		wfmo->StillWaiting = false;
		
		--wfmo->RefCount;
		if(wfmo->RefCount == 0)
		{
			wfmo->Destroy();
			delete wfmo;
		}
		else
		{
			int tempResult = pthread_mutex_unlock(&wfmo->Mutex);
#ifdef PTHREADCHK
			if (tempResult != 0)
			{
				//Inconsistent state. Cannot continue after this.
				assert(false);
				return tempResult;
			}
#endif
		}
		
		return result;
	}
#endif
	
	int DestroyEvent(neosmart_event_t event)
	{
		int result = pthread_cond_destroy(&event->CVariable);
#ifdef PTHREADCHK
		if(result != 0)
			return result;
#endif
        
		result = pthread_mutex_destroy(&event->Mutex);
#ifdef PTHREADCHK
		if(result != 0)
			return result;
#endif
		
		delete event;
		
		return 0;
	}
	
	int SetEvent(neosmart_event_t event)
	{
		int result = pthread_mutex_lock(&event->Mutex);
#ifdef PTHREADCHK
		if(result != 0)
			return result;
#endif
		
		event->State = true;
		
		//Depending on the event type, we either trigger everyone or only one
		if(event->AutoReset)
		{
#ifdef WFMO
			while(!event->RegisteredWaits.empty())
			{
				neosmart_wfmo_info_t i = &event->RegisteredWaits.front();
				pthread_mutex_lock(&i->Waiter->Mutex);
				--i->Waiter->RefCount;
				if(!i->Waiter->StillWaiting)
				{
					if(i->Waiter->RefCount == 0)
					{
						i->Waiter->Destroy();
						delete i->Waiter;
					}
					else
					{
						pthread_mutex_unlock(&i->Waiter->Mutex);
					}
					event->RegisteredWaits.pop_front();
					continue;
				}
				
				event->State = false;
				
				if(i->Waiter->WaitAll)
				{
					--i->Waiter->Status.EventsLeft;
					assert(i->Waiter->Status.EventsLeft >= 0);
					//We technically should do i->Waiter->StillWaiting = Waiter->Status.EventsLeft != 0
					//but the only time it'll be equal to zero is if we're the last event, so no one
					//else will be checking the StillWaiting flag. We're good to go without it.
				}
				else
				{
					i->Waiter->Status.FiredEvent = i->WaitIndex;
					i->Waiter->StillWaiting = false;
				}
				
				pthread_mutex_unlock(&i->Waiter->Mutex);
				result = pthread_cond_signal(&i->Waiter->CVariable);
				event->RegisteredWaits.pop_front();
				pthread_mutex_unlock(&event->Mutex);
				
				return result;
			}
#endif
			//event->State can be false if compiled with WFMO support
			if(event->State)
			{
				int tempResult = pthread_mutex_unlock(&event->Mutex);
#ifdef PTHREADCHK
				if (tempResult != 0)
				{
					//Inconsistent state. Cannot continue after this.
					assert(false);
					return tempResult;
				}
#endif
				result = pthread_cond_signal(&event->CVariable);
				
				return result;
			}
		}
		else
		{
#ifdef WFMO
			for(size_t i = 0; i < event->RegisteredWaits.size(); ++i)
			{
				neosmart_wfmo_info_t info = &event->RegisteredWaits[i];
				int tempResult = pthread_mutex_lock(&info->Waiter->Mutex);
#ifdef PTHREADCHK
				if (tempResult != 0)
				{
					//Inconsistent state. Cannot continue after this.
					assert(false);
					return tempResult;
				}
#endif
				--info->Waiter->RefCount;
				if(!info->Waiter->StillWaiting)
				{
					if(info->Waiter->RefCount == 0)
					{
						info->Waiter->Destroy();
						delete info->Waiter;
					}
					else
					{
						tempResult = pthread_mutex_unlock(&info->Waiter->Mutex);
#ifdef PTHREADCHK
						if (tempResult != 0)
						{
							//Inconsistent state. Cannot continue after this.
							assert(false);
							return tempResult;
						}
#endif
					}
					continue;
				}
				
				if(info->Waiter->WaitAll)
				{
					--info->Waiter->Status.EventsLeft;
					assert(info->Waiter->Status.EventsLeft >= 0);
					//We technically should do i->Waiter->StillWaiting = Waiter->Status.EventsLeft != 0
					//but the only time it'll be equal to zero is if we're the last event, so no one
					//else will be checking the StillWaiting flag. We're good to go without it.
				}
				else
				{
					info->Waiter->Status.FiredEvent = info->WaitIndex;
					info->Waiter->StillWaiting = false;
				}
				
				pthread_mutex_unlock(&info->Waiter->Mutex);
				pthread_cond_signal(&info->Waiter->CVariable);
			}
			event->RegisteredWaits.clear();
#endif
			int tempResult = pthread_mutex_unlock(&event->Mutex);
#ifdef PTHREADCHK
			if (tempResult != 0)
			{
				//Inconsistent state. Cannot continue after this.
				assert(false);
				return tempResult;
			}
#endif
			result = pthread_cond_broadcast(&event->CVariable);
		}
		
		return result;
	}
	
	int ResetEvent(neosmart_event_t event)
	{
		int result = pthread_mutex_lock(&event->Mutex);
#ifdef PTHREADCHK
		if(result != 0)
			return result;
#endif
		
		event->State = false;
		
		int tempResult = pthread_mutex_unlock(&event->Mutex);
#ifdef PTHREADCHK
		if (tempResult != 0)
		{
			//Inconsistent state. Cannot continue after this.
			assert(false);
			return tempResult;
		}
#endif
		
		return result;
	}
}
