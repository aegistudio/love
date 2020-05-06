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

// LOVE
#include "ReadyQueue.h"
#include "Exception.h"
#include "runtime.h"

// C++
#include <utility>
#include <memory>
#include <stdexcept>

// likely and unlikely primitive to indicate branch prediction.
#if (defined(__GNUC__) && (__GNUC__ >= 3)) || (defined(__INTEL_COMPILER) && (__INTEL_COMPILER >= 800)) || defined(__clang__)
#  define expect(expr,value)    (__builtin_expect ((expr),(value)) )
#else
#  define expect(expr,value)    (expr)
#endif

#define likely(expr)     expect((expr) != 0, 1)
#define unlikely(expr)   expect((expr) != 0, 0)

namespace love
{

/**
 * w__keyReadyQueue item randomly generates a pointer address in memory for
 * storing the ready queue object pointer into there. Lower 32-bit of the
 * value address will be taken in order to avoid the lightuserdata related
 * problem inside LuaJIT.
 **/
static void* w__keyReadyQueue() noexcept {
	static long randomSpace;
	union {
		void* addr;
		int lower32Bit;
	} randomAddr;
	randomAddr.addr = reinterpret_cast<void*>(&randomSpace);
	return reinterpret_cast<void*>(randomAddr.lower32Bit);
}

ReadyQueue::ReadyQueue(lua_State* L) : L(L) {
	// Allocate the lua space information related to the ready queue.
	void* rqkey = w__keyReadyQueue();
	lua_pushlightuserdata(L, rqkey);
	lua_createtable(L, 3, 0); // [] rqkey rq

	// Bind the current ready queue instance to the main state.
	love::luax_pushpointerasstring(L, reinterpret_cast<void*>(this));
	lua_rawseti(L, -2, 1); // [] rqkey rq

	// Bind the current allocation information to the table.
	lua_newtable(L); // [] rqkey rq alloc
	lua_rawseti(L, -2, 2); // [] rqkey rq

	// Bind the current registration information to the table.
	lua_newtable(L); // [] rqkey rq reg
	lua_rawseti(L, -2, 3); // [] rqkey rq

	// Set the table to the given address by the lightuserdata.
	lua_rawset(L, LUA_REGISTRYINDEX); // []
}

ReadyQueue::~ReadyQueue() {
	// Unbind the previously bind ready queue registry index.
	void* rqkey = w__keyReadyQueue();
	lua_pushlightuserdata(L, rqkey);
	lua_pushnil(L);
	lua_rawset(L, LUA_REGISTRYINDEX);
}

/**
 * w__readyQueueLuaLoad will attempt to load the lua table whose
 * key is w__keyReadyQueue(), and push it onto the lua stack.
 *
 * Exception will be thrown if there's no such table found.
 **/
static void w__readyQueueLuaLoad(lua_State* L) {
	void* rqkey = w__keyReadyQueue();
	lua_pushlightuserdata(L, rqkey); // [] rqkey
	lua_rawget(L, LUA_REGISTRYINDEX); // [] rq
	if(unlikely(!lua_istable(L, -1))) {
		throw love::Exception("Not managed by ready queue.");
	}
	if(unlikely(lua_objlen(L, -1) != 3)) {
		throw love::Exception("Ready queue lua table corrupted.");
	}
}

/**
 * w__allocReferenceOf retrieve the reference to the specified
 * thread object. LUA_NOREF will be returned if the specified
 * object is not inside the allocation table.
 **/
lua_Integer w__allocReferenceOf(lua_State* L) {
	lua_Integer reference = LUA_NOREF;
	// Check whether there's an allocation associated with
	// the specified lua state.
	lua_rawgeti(L, -1, 2); // [] rq alloc
	love::luax_pushpointerasstring(L, reinterpret_cast<void*>(L));
	lua_rawget(L, -2); // [] rq alloc reference
	if(likely(lua_isnumber(L, -1))) {
		reference = lua_tointeger(L, -1);
	}
	lua_pop(L, 2); // [] rq
	return reference;
}

int luax_yieldrq(lua_State* L, luax_yieldfunc f) {
	love::luax_catchexcept(L, [&]() {
		// Transfer the ownership of function f into the object.
		luax_yieldfunc g;
		std::swap(f, g);

		// Load and verify the yieldability of the coroutine.
		w__readyQueueLuaLoad(L); // [] rq
		lua_Integer ref = w__allocReferenceOf(L); // [] rq
		if(ref == LUA_NOREF) {
			throw love::Exception("Not a yieldable coroutine.");
		}

		// Okay, now we know the lua state has been valid for
		// allocation, and the lua reference for it has been placed
		// inside the allocation, retrieve the reference and call
		// the yield function. Before calling the yield function,
		// the stack must be recovered to the state before calling
		// the yield function.
		lua_pop(L, 1); // []
		f(L, ref);
	});

	// Now there's no long jump from the luax_catchexcept, which
	// means there's no error found while yielding. So we will
	// just clear the content of the stack and yield out to caller.
	//
	// XXX: the light pointer will be pushed onto the stack as
	// the magic number to identify coroutines yielded using
	// luax_yieldrq, rather than using the coroutine.yield from
	// the lua side. (This shall affects how the scheduler treats
	// the coroutines).
	lua_settop(L, 0);
	void* yieldmn = w__keyReadyQueue();
	lua_pushlightuserdata(L, yieldmn);
	return lua_yield(L, 1);
}

void luax_resumerq(lua_State* L, lua_Integer ref, luax_resumefunc f) {
	love::luax_catchexcept(L, [&] {
		// Take the ownership of the resume function here.
		luax_resumefunc g;
		std::swap(f, g);

		// Attempt to load the ready queue instance onto the stack.
		w__readyQueueLuaLoad(L); // [] rq

		// Perform extra checking when the object is not LUA_NOREF,
		// which refernces to a managed coroutine.
		if(ref != LUA_NOREF) {
			// Attempt to lookup the lua state corresponding to the
			// given reference.
			lua_rawgeti(L, -1, 3); // [] rq reg
			lua_rawgeti(L, -1, ref); // [] rq reg thread
			if(unlikely(!lua_isthread(L, -1))) {
				throw love::Exception("Invalid coroutine reference.");
			}

			// Attempt to see whether the specified thread is managed.
			lua_State* co = lua_tothread(L, -1);
			lua_pop(L, 2); // [] rq
			if(unlikely(w__allocReferenceOf(co) != ref)) {
				throw love::Exception("Invalid coroutine reference.");
			}
		}

		// Now the coroutine is valid for yielding, now we will
		// fetch the ready queue object.
		lua_rawgeti(L, -1, 1); // [] rq object
		ReadyQueue* result = nullptr;
		size_t length = sizeof(void*);
		const char* value = lua_tolstring(L, -1, &length);
		memcpy(reinterpret_cast<void*>(&result), value, length);
		lua_pop(L, 1); // [] rq
		if(unlikely(result == nullptr)) {
			throw love::Exception("Invalid nullptr ready queue object.");
		}

		// We will enqueue the item into the ready queue so that
		// it will be scheduled later.
		ReadyQueue::Item newrqi;
		newrqi.handle = ref;
		std::swap(newrqi.f, g);
		result->rq.push_back(std::move(newrqi));
	});
}

void ReadyQueue::schedule() {
	// Yielded are the items that has yielded themselves using the
	// coroutine.yield function will be added here. They will not
	// be scheduled anymore except when the ReadyQueue::schedule
	// function is called again.
	std::list<ReadyQueue::Item> yielded;
	std::shared_ptr<void> yieldedDefer(nullptr, [&] (void*) {
		// The yielded list will be moved into the list tail.
		rq.splice(rq.end(), yielded);
	});

	// Resume the caller stack when it exits, no matter what has
	// encountered while calling schedule.
	int calltop = lua_gettop(L); // []
	std::shared_ptr<void> topDefer(nullptr, [&] (void*) {
		lua_settop(L, calltop);
	});

	// Initialize th ready queue object on the stack, this will also
	// reenable the resolution of the lua stack.
	w__readyQueueLuaLoad(L); // [] rq
	int rqtop = lua_gettop(L);

	// Iterate and remove item from the ready queue, until there's
	// no item available for being scheduled, or there's fatal exception
	// thrown from the schedule function.
	while(!rq.empty()) {
		// Setup a new scheduling context for the ready queue head.
		ReadyQueue::Item item = std::move(rq.front());
		rq.pop_front();
		std::shared_ptr<void> itemDefer(nullptr, [&] (void*) {
			// The identity allocated for the item will be
			// removed if its specified handle is valid.
			if(item.handle != LUA_NOREF) {
				lua_settop(L, rqtop); // [] rq

				// Similar to the code in luax_resume, but this
				// time we will actually make use of this data.
				lua_rawseti(L, -1, 3); // [] rq reg
				lua_rawgeti(L, -1, item.handle); // [] rq reg thread
				if(unlikely(!lua_isthread(L, -1))) {
					throw love::Exception("Invalid coroutine reference.");
				}
				void* coptr = reinterpret_cast<void*>(lua_tothread(L, -1));
				lua_pop(L, 1); // [] rq reg

				// Of course, we will erase the data of this
				// reference here first.
				luaL_unref(L, -1, item.handle);
				lua_pop(L, 1); // [] rq

				// Then we shall erase the item from the allocation table.
				lua_rawgeti(L, -1, 2); // [] rq alloc
				love::luax_pushpointerasstring(L, reinterpret_cast<void*>(L));
				lua_pushnil(L);  // [] rq alloc reference nil
				lua_rawset(L, -3); // [] rq alloc

				// The coroutine associated with the item has
				// been destroyed now, so we will simply move on.
				lua_pop(L, 1); // [] rq
			}
		});

		// Clear the main stack to reserve place for the coroutine.
		lua_settop(L, rqtop); // [] rq

		// We commonly requires the registration lookup in the first
		// few steps, so we load it here.
		lua_rawgeti(L, -1, 3); // [] rq reg

		// Create the new coroutine handle if the specified ready item
		// references to the LUA_NOREF handle.
		if(item.handle == LUA_NOREF) {
			void* coptr = reinterpret_cast<void*>(lua_newthread(L));
			int ref = luaL_ref(L, -2); // [] rq reg
			lua_rawgeti(L, -2, 2); // [] rq reg alloc
			love::luax_pushpointerasstring(L, coptr); // [] rq reg alloc coptr
			lua_pushinteger(L, ref); // [] rq reg alloc coptr ref
			lua_rawset(L, -3); // [] rq reg alloc
			item.handle = ref;
			lua_pop(L, 1); // [] rq reg
		}

		// Acquire the specified coroutine from the item.
		lua_rawgeti(L, -1, item.handle); // [] rq reg co

		// Resume the ready queue items into the main state, and
		// collect their execution result.
		int nresume = item.f(L);
		int code = love::luax_resume(L, nresume);
		switch(code) {
		case LUA_YIELD: {
			// The coroutine has been yielded, waiting for further
			// execution, so we will simply eat up the yielded value
			// and check whether to emplace it back to the yielded.
			lua_Integer handle = LUA_NOREF;
			std::swap(item.handle, handle);
			if(lua_isuserdata(L, -1)) {
				void* yieldmn = w__keyReadyQueue();
				void* data = lua_touserdata(L, -1);
				if(yieldmn == data) {
					// The item is yielded using luax_yieldrq, so
					// we will not add it back to the ready queue,
					// but just continue on instead.
					continue;
				}
			}

			// Add the coroutine back to the yielded queue, if it
			// is yielded using other function instead.
			ReadyQueue::Item yieldrqi;
			yieldrqi.handle = handle;
			yieldrqi.f = [] (lua_State* L) {
				// We will resume nothing when expected this kind
				// of yielding, according to the definition.
				return 0;
			};
			yielded.push_back(std::move(yieldrqi));

		}; break;
		case LUA_ERRMEM:
			// TODO: Push the error message of out of memory back
			// to the stack top here.
		case LUA_ERRERR:
		case LUA_ERRRUN:
			// TODO: There's error while executing the coroutine, we
			// are intended to store the error code somewhere and
			// forward it to the main loop later.
		case 0:
			// The coroutine has finished its execution, so it should
			// be handled as if there's nothing to do more.
			break;
		default:
			throw love::Exception("Unrecognized resume code expected");
		}
	}
}

} // love
