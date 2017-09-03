/*************************************************************************
* Detouring::ClassProxy
* A C++ class that allows you to "proxy" virtual tables and receive
* calls in substitute classes. Contains helpers for detouring regular
* member functions as well.
*------------------------------------------------------------------------
* Copyright (c) 2017, Daniel Almeida
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following conditions
* are met:
*
* 1. Redistributions of source code must retain the above copyright
* notice, this list of conditions and the following disclaimer.
*
* 2. Redistributions in binary form must reproduce the above copyright
* notice, this list of conditions and the following disclaimer in the
* documentation and/or other materials provided with the distribution.
*
* 3. Neither the name of the copyright holder nor the names of its
* contributors may be used to endorse or promote products derived from
* this software without specific prior written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
* A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
* HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
* SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
* LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
* DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
* THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*************************************************************************/

/*************************************************************************
* Implementation of the Detouring::GetVirtualAddress function heavily
* based on meepdarknessmeep's vhook.h header at
* https://github.com/glua/gm_fshook/blob/master/src/vhook.h
* Thanks a lot, xoxo.
*************************************************************************/

#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <utility>
#include "hook.hpp"

#ifdef _WIN32

#define CLASSPROXY_THISCALL __thiscall

#else

#define CLASSPROXY_THISCALL

#endif

namespace Detouring
{
	enum class MemberType
	{
		Static,
		NonVirtual,
		Virtual
	};

	struct Member
	{
		Member( );
		Member( size_t idx, void *addr );

		void *address;
		size_t index;
	};

	typedef std::unordered_map<void *, Member> CacheMap;
	typedef std::unordered_map<void *, Detouring::Hook> HookMap;

	template<typename Class>
	inline void **GetVirtualTable( Class *instance )
	{
		return *reinterpret_cast<void ***>( instance );
	}

	template<typename RetType, typename Class, typename... Args>
	inline void *GetAddress( RetType ( Class::* method )( Args... ) )
	{
		RetType ( Class::** pmethod )( Args... ) = &method;

#ifdef _MSC_VER

		void *address = *reinterpret_cast<void **>( pmethod );

#else

		void *address = reinterpret_cast<void *>( pmethod );

#endif

		// Check whether the function starts with a relative far jump and assume a debug compilation thunk
		uint8_t *method_code = reinterpret_cast<uint8_t *>( address );
		if( method_code[0] == 0xE9 )
			address = method_code + 5 + *reinterpret_cast<int32_t *>( method_code + 1 );

		return address;
	}

	// Can be used with interfaces and implementations
	template<typename RetType, typename Class, typename... Args>
	inline Member GetVirtualAddress(
		void **vtable,
		size_t size,
		RetType ( Class::* method )( Args... )
	)
	{
		if( vtable == nullptr || size == 0 || method == nullptr )
			return Member( );

#ifdef _MSC_VER

		void *member = GetAddress( method );
		uint8_t *addr = reinterpret_cast<uint8_t *>( member );

#ifdef _WIN64

		// x86-64, mov rax, [rcx]
		if( addr[0] == 0x48 )
			addr += 3;

#else

		if( addr[0] == 0x8B )
			addr += 2;

#endif

		// check for jmp functions
		if( addr[0] == 0xFF && ( ( addr[1] >> 4 ) & 3 ) == 2 )
		{
			uint8_t jumptype = addr[1] >> 6;
			uint32_t offset = 0;
			if( jumptype == 1 ) // byte
				offset = addr[2];
			else if( jumptype == 2 )
				offset = *reinterpret_cast<uint32_t *>( &addr[2] );

			size_t index = offset / sizeof( void * );
			if( index >= size )
				return Member( );

			return Member( index, vtable[index] );
		}

		for( size_t index = 0; index < size; ++index )
			if( vtable[index] == member )
				return Member( index, member );

		return Member( );

#else

		RetType ( Class::** pmethod )( Args... ) = &method;
		void *address = *reinterpret_cast<void **>( pmethod );
		size_t offset = ( reinterpret_cast<uintptr_t>( address ) - 1 ) / sizeof( void * );
		if( offset >= size )
		{
			for( size_t index = 0; index < size; ++index )
				if( vtable[index] == address )
					return Member( index, address );

			return Member( );
		}

		return Member( offset, vtable[offset] );

#endif

	}

	bool ProtectMemory( void *pMemory, size_t uiLen, bool protect );

	bool IsExecutableAddress( void *pAddress );

	template<typename Target, typename Substitute>
	class ClassProxy
	{
	protected:
		ClassProxy( )
		{ }

		ClassProxy( Target *instance )
		{
			Initialize( instance );
		}

		virtual ~ClassProxy( )
		{
			if( target_vtable != nullptr && target_size != 0 )
			{
				ProtectMemory( target_vtable, target_size * sizeof( void * ), false );

				void **vtable = target_vtable;
				for(
					auto it = original_vtable.begin( );
					it != original_vtable.end( );
					++vtable, ++it
				)
					if( *vtable != *it )
						*vtable = *it;

				ProtectMemory( target_vtable, target_size * sizeof( void * ), true );
			}
		}

	public:
		static bool Initialize( Target *instance, Substitute *substitute )
		{
			if( target_vtable != nullptr )
				return false;

			target_vtable = GetVirtualTable( instance );
			if( target_vtable == nullptr || !IsExecutableAddress( *target_vtable ) )
			{
				target_vtable = nullptr;
				return false;
			}

			for(
				void **vtable = target_vtable;
				original_vtable.size( ) < original_vtable.max_size( ) && *vtable != nullptr;
				++vtable
			)
				original_vtable.push_back( *vtable );

			target_size = original_vtable.size( );

			substitute_vtable = GetVirtualTable( substitute );

			for( ; substitute_vtable[substitute_size] != nullptr; ++substitute_size );

			return true;
		}

		inline bool Initialize( Target *instance )
		{
			return Initialize( instance, static_cast<Substitute *>( this ) );
		}

		inline Target *This( )
		{
			return reinterpret_cast<Target *>( this );
		}

		template<typename RetType, typename... Args>
		static bool IsHooked( RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ) )
		{
			return IsHookedInternal( original );
		}

		template<typename RetType, typename... Args>
		static bool IsHooked( RetType ( Target::* original )( Args... ) )
		{
			return IsHookedInternal( original );
		}

		template<typename RetType, typename... Args>
		static bool IsHooked( RetType ( Target::* original )( Args... ) const )
		{
			return IsHookedInternal(
				reinterpret_cast<RetType ( Target::* )( Args... )>( original )
			);
		}

		template<typename RetType, typename... Args>
		static bool Hook(
			RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ),
			RetType ( Substitute::* substitute )( Args... )
		)
		{
			return HookInternal( original, substitute );
		}

		template<typename RetType, typename... Args>
		static bool Hook(
			RetType ( Target::* original )( Args... ),
			RetType ( Substitute::* substitute )( Args... )
		)
		{
			return HookInternal( original, substitute );
		}

		template<typename RetType, typename... Args>
		static bool Hook(
			RetType ( Target::* original )( Args... ) const,
			RetType ( Substitute::* substitute )( Args... ) const
		)
		{
			return HookInternal(
				reinterpret_cast<RetType ( Target::* )( Args... )>( original ),
				reinterpret_cast<RetType ( Target::* )( Args... )>( substitute )
			);
		}

		template<typename RetType, typename... Args>
		static bool UnHook( RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ) )
		{
			return UnHookInternal( original );
		}

		template<typename RetType, typename... Args>
		static bool UnHook( RetType ( Target::* original )( Args... ) )
		{
			return UnHookInternal( original );
		}

		template<typename RetType, typename... Args>
		static bool UnHook( RetType ( Target::* original )( Args... ) const )
		{
			return UnHookInternal(
				reinterpret_cast<RetType ( Target::* )( Args... )>( original )
			);
		}

		template<typename RetType, typename... Args>
		static RetType Call(
			Target *instance,
			RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ),
			Args... args
		)
		{
			return CallInternal<RetType, Args...>( instance, original, std::forward<Args>( args )... );
		}

		template<typename RetType, typename... Args>
		static RetType Call(
			Target *instance,
			RetType ( Target::* original )( Args... ),
			Args... args
		)
		{
			return CallInternal<RetType, Args...>( instance, original, std::forward<Args>( args )... );
		}

		template<typename RetType, typename... Args>
		static RetType Call(
			Target *instance,
			RetType ( Target::* original )( Args... ) const,
			Args... args
		)
		{
			return CallInternal<RetType, Args...>(
				instance,
				reinterpret_cast<RetType ( Target::* )( Args... )>( original ),
				std::forward<Args>( args )...
			);
		}

		template<typename RetType, typename... Args>
		inline RetType Call( RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ), Args... args )
		{
			return Call<RetType, Args...>(
				reinterpret_cast<Target *>( this ), original, std::forward<Args>( args )...
			);
		}

		template<typename RetType, typename... Args>
		inline RetType Call( RetType ( Target::* original )( Args... ), Args... args )
		{
			return Call<RetType, Args...>(
				reinterpret_cast<Target *>( this ), original, std::forward<Args>( args )...
			);
		}

		template<typename RetType, typename... Args>
		inline RetType Call( RetType ( Target::* original )( Args... ) const, Args... args )
		{
			return Call<RetType, Args...>(
				reinterpret_cast<Target *>( this ), original, std::forward<Args>( args )...
			);
		}

		template<typename RetType, typename... Args>
		static Member GetTargetVirtualAddress( RetType ( Target::* method )( Args... ) )
		{
			return GetVirtualAddressInternal(
				target_cache, target_vtable, target_size, method
			);
		}

		template<typename RetType, typename... Args>
		static Member GetTargetVirtualAddress( RetType ( Target::* method )( Args... ) const )
		{
			return GetVirtualAddressInternal(
				target_cache, target_vtable, target_size, method
			);
		}

		template<typename RetType, typename... Args>
		static Member GetSubstituteVirtualAddress( RetType ( Substitute::* method )( Args... ) )
		{
			return GetVirtualAddressInternal(
				substitute_cache,
				substitute_vtable,
				substitute_size,
				method
			);
		}

		template<typename RetType, typename... Args>
		static Member GetSubstituteVirtualAddress( RetType ( Substitute::* method )( Args... ) const )
		{
			return GetVirtualAddressInternal(
				substitute_cache,
				substitute_vtable,
				substitute_size,
				method
			);
		}

	private:
		// can be used with interfaces and implementations
		template<typename RetType, typename Class, typename... Args>
		static Member GetVirtualAddressInternal(
			CacheMap &cache,
			void **vtable,
			size_t size,
			RetType ( Class::* method )( Args... )
		)
		{
			void *member = GetAddress( method );
			auto it = cache.find( member );
			if( it != cache.end( ) )
				return ( *it ).second;

			Member address = GetVirtualAddress( vtable, size, method );

			if( address.index < size )
				cache[member] = address;

			return address;
		}

		template<typename RetType, typename... Args>
		static bool IsHookedInternal( RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ) )
		{
			return hooks.find( original ) != hooks.end( );
		}

		template<typename RetType, typename... Args>
		static bool IsHookedInternal( RetType ( Target::* original )( Args... ) )
		{
			auto it = hooks.find( GetAddress( original ) );
			if( it != hooks.end( ) )
				return true;

			Member vtarget = GetTargetVirtualAddress( original );
			if( vtarget.index >= target_size )
				return false;

			return target_vtable[vtarget.index] != original_vtable[vtarget.index];
		}

		template<typename RetType, typename... Args>
		static bool HookInternal(
			RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ),
			RetType ( Substitute::* substitute )( Args... )
		)
		{
			void *address = original;
			if( address == nullptr )
				return false;

			auto it = hooks.find( address );
			if( it != hooks.end( ) )
				return false;

			void *subst = GetAddress( substitute );
			if( subst == nullptr )
				return false;

			Detouring::Hook &hook = hooks[address];
			if( !hook.Create( address, subst ) )
				return false;

			return hook.Enable( );
		}

		template<typename RetType, typename... Args>
		static bool HookInternal(
			RetType ( Target::* original )( Args... ),
			RetType ( Substitute::* substitute )( Args... )
		)
		{
			Member target = GetTargetVirtualAddress( original );
			if( target.index < target_size )
			{
				if( target_vtable[target.index] != original_vtable[target.index] )
					return false;

				Member subst = GetSubstituteVirtualAddress( substitute );
				if( subst.index >= substitute_size )
					return false;

				ProtectMemory( target_vtable + target.index, sizeof( void * ), false );
				target_vtable[target.index] = subst.address;
				ProtectMemory( target_vtable + target.index, sizeof( void * ), true );

				return true;
			}

			void *address = GetAddress( original );
			if( address == nullptr )
				return false;

			auto it = hooks.find( address );
			if( it != hooks.end( ) )
				return false;

			void *subst = GetAddress( substitute );
			if( subst == nullptr )
				return false;

			Detouring::Hook &hook = hooks[address];
			if( !hook.Create( address, subst ) )
				return false;

			return hook.Enable( );
		}

		template<typename RetType, typename... Args>
		static bool UnHookInternal( RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ) )
		{
			auto it = hooks.find( original );
			if( it != hooks.end( ) )
			{
				hooks.erase( it );
				return true;
			}

			return false;
		}

		template<typename RetType, typename... Args>
		static bool UnHookInternal( RetType ( Target::* original )( Args... ) )
		{
			auto it = hooks.find( GetAddress( original ) );
			if( it != hooks.end( ) )
			{
				hooks.erase( it );
				return true;
			}

			Member target = GetTargetVirtualAddress( original );
			if( target.index >= target_size )
				return false;

			void *vfunction = original_vtable[target.index];
			if( target_vtable[target.index] == vfunction )
				return false;

			ProtectMemory( target_vtable + target.index, sizeof( void * ), false );
			target_vtable[target.index] = vfunction;
			ProtectMemory( target_vtable + target.index, sizeof( void * ), true );

			return true;
		}

		template<typename RetType, typename... Args>
		static RetType CallInternal(
			Target *instance,
			RetType ( CLASSPROXY_THISCALL *original )( Target *, Args... ),
			Args... args
		)
		{
			Member target;
			void *address = original;
			auto it = hooks.find( address );
			if( it != hooks.end( ) )
			{
				target.address = ( *it ).second.GetTrampoline( );
				if( target.address != nullptr )
					target.index = 0;
			}

			if( target.index >= target_size )
			{
				target.address = address;
				if( target.address != nullptr )
					target.index = 0;
			}

			if( target.index >= target_size )
				return RetType( );

			auto method = reinterpret_cast<RetType ( * )( Target *, Args... )>( target.address );
			return method( instance, std::forward<Args>( args )... );
		}

		template<typename RetType, typename... Args>
		static RetType CallInternal(
			Target *instance,
			RetType ( Target::* original )( Args... ),
			Args... args
		)
		{
			Member target;
			void *address = GetAddress( original );
			auto it = hooks.find( address );
			if( it != hooks.end( ) )
			{
				target.address = ( *it ).second.GetTrampoline( );
				if( target.address != nullptr )
					target.index = 0;
			}

			if( target.index >= target_size )
			{
				target = GetTargetVirtualAddress( original );
				if( target.index < target_size )
					target.address = original_vtable[target.index];
			}

			if( target.index >= target_size )
			{
				target.address = address;
				if( target.address != nullptr )
					target.index = 0;
			}

			if( target.index >= target_size )
				return RetType( );

			auto typedfunc = reinterpret_cast<RetType ( Target::** )( Args... )>( &target );
			return ( instance->**typedfunc )( std::forward<Args>( args )... );
		}

		static size_t target_size;
		static void **target_vtable;
		static CacheMap target_cache;
		static std::vector<void *> original_vtable;
		static size_t substitute_size;
		static void **substitute_vtable;
		static CacheMap substitute_cache;
		static HookMap hooks;
	};

	template<typename Target, typename Substitute>
	size_t ClassProxy<Target, Substitute>::target_size = 0;
	template<typename Target, typename Substitute>
	void **ClassProxy<Target, Substitute>::target_vtable = nullptr;
	template<typename Target, typename Substitute>
	CacheMap ClassProxy<Target, Substitute>::target_cache;
	template<typename Target, typename Substitute>
	std::vector<void *> ClassProxy<Target, Substitute>::original_vtable;
	template<typename Target, typename Substitute>
	size_t ClassProxy<Target, Substitute>::substitute_size = 0;
	template<typename Target, typename Substitute>
	void **ClassProxy<Target, Substitute>::substitute_vtable = nullptr;
	template<typename Target, typename Substitute>
	CacheMap ClassProxy<Target, Substitute>::substitute_cache;
	template<typename Target, typename Substitute>
	HookMap ClassProxy<Target, Substitute>::hooks;
}