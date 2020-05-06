/**
 * Copyright (c) 2006-2020 LOVE Development Team
 *
 * This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the authors be held liable for any damages
 * arising from the use of this software.
 *
 * Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *
 * 1. The origin of this software must not be misrepresented; you must not
 *    claim that you wrote the original software. If you use this software
 *    in a product, an acknowledgment in the product documentation would be
 *    appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 **/

#ifndef LOVE_READY_QUEUE_H
#define LOVE_READY_QUEUE_H

// LOVE
#include "config.h"

// Lua
extern "C" {
	#define LUA_COMPAT_ALL
	#include <lua.h>
	#include <lualib.h>
	#include <lauxlib.h>
}

// C++
#include <list>
#include <functional>

namespace love
{

/**
 * Resume function is used to initialize or resume the lua stack
 * before the scheduler yielding back to the lua stack.
 **/
using luax_resumefunc = std::function<int(lua_State*)>;

/**
 * Initialize or push a coroutine back to the ready queue. If the
 * coroutine handle is LUA_NOREF, a new coroutine will be created,
 * or if the coroutine handle is one generated and passed into the
 * luax_yieldrq, the specified coroutine will be resumed.
 *
 * Exception will be thrown if it is not managed by a ready queue,
 * and the function returns immediately without control transferring.
 **/
LOVE_EXPORT void luax_resumerq(
	lua_State* L, lua_Integer handle, luax_resumefunc f);

/**
 * ReadyQueue is a caller managed queue used for collecting ready
 * coroutines, manage their execution, yielding and resuming whenever
 * it is scheduled to do so.
 *
 * Please notice this instance does not subsume to the lifecycle
 * management provided by love::Object since it always lives longer
 * than all created lua coroutines.
 **/
class ReadyQueue {
	/**
	 * Item represents an ready queue item that should be managed
	 * by the ready queue. It contains the target coroutine to
	 * resume and the resume function to initialize the stack.
	 **/
	struct Item {
		// Handle to the coroutine to resume its execution.
		lua_Integer handle;

		// Closure for preparing or resuming the coroutine stack.
		luax_resumefunc f;
	};

	/**
	 * Singly-linked queue storing the ready items which will be
	 * used for scheduling later in the schedule function.
	 **/
	std::list<Item> rq;

	/**
	 * Associated state that should be used for scheduling the
	 * threads inside state. The state is usually the main state.
	 **/
	lua_State* L;

	// Make the resumerq function a friend of this class, in order
	// to make it possible for manipulating the ready queue.
	friend void luax_resumerq(
		lua_State* L, lua_Integer handle, luax_resumefunc f);
public:
	/**
	 * Associated main lua_State which is used for coroutine managing
	 * and scheduling.
	 **/
	LOVE_EXPORT ReadyQueue(lua_State* L);

	/**
	 * Destroy the ready queue as well as leaving all ready and
	 * created coroutine at destroyed state.
	 **/
	LOVE_EXPORT ~ReadyQueue();

	/**
	 * Schedule the items inside the ready queue. This function must
	 * only be called when the main state is not running.
	 *
	 * Obviously this function should only be called from the main
	 * thread / main loop. This function is mostly non-blocking
	 * unless there's dead loop inside one of the scheduled thread.
	 **/
	LOVE_EXPORT void schedule();
};

/**
 * Yield function is used to prepare delayed resume with a coroutine
 * handle. The coroutine handle must be managed properly by the
 * underlying function otherwise memory leak might be caused.
 */
using luax_yieldfunc = std::function<void(lua_State* L, lua_Integer)>;

/**
 * Yield the current goroutine back to the scheduler. This function
 * should be the last function when you call return. Just like how
 * you use the lua_yield function.
 *
 * Exception will be thrown if the current thread to yield is a main
 * state. And the thread will be automatically GC-ed if there's no
 * ready queue bound to the state.
 **/
LOVE_EXPORT int luax_yieldrq(lua_State* L, luax_yieldfunc f);

} //love

#endif// LOVE_READY_QUEUE_H
