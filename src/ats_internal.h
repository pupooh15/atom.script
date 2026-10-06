/**************************************************************************/
/*!	\file	ats_internal.h
	\brief	AtomScript コア内部定義
	\note
	例外・RTTI・STL を使わない。メモリはすべてホストのアロケータから取る。
***************************************************************************/
#ifndef ATS_INTERNAL_H
#define ATS_INTERNAL_H

#include "atomscript/ats_api.h"
#include "atomscript/ats_format.h"

#include <stdarg.h>
#include <string.h>
#include <new>
#include <type_traits>

namespace ats {

//=========================================================================
// アロケータ
//=========================================================================
struct Allocator {
	ats_alloc_fn	alloc;
	ats_free_fn		free;
	void*			user;

	void* Alloc( size_t size, size_t align = 16 ) const { return alloc( size, align, user ); }
	void  Free( void* p ) const { if( p ) free( p, user ); }

	template<class T, class... A> T* New( A&&... args ) const
	{
		void* p = Alloc( sizeof(T), alignof(T) );
		return p ? new(p) T( static_cast<A&&>(args)... ) : nullptr;
	}
	template<class T> void Delete( T* p ) const
	{
		if( !p ) return;
		p->~T();
		Free( p );
	}
	char* StrDup( const char* s ) const
	{
		size_t n = strlen( s ) + 1;
		char* d = (char*)Alloc( n, 1 );
		if( d ) memcpy( d, s, n );
		return d;
	}
};

//=========================================================================
// 可変長配列（トリビアルコピー可能な型のみ）
//=========================================================================
template<class T>
class Array {
	static_assert( std::is_trivially_copyable<T>::value, "Array<T> requires trivially copyable T" );
public:
	Array() = default;
	Array( const Array& ) = delete;
	Array& operator=( const Array& ) = delete;
	~Array() { Reset(); }

	void Init( const Allocator* a ) { m_alloc = a; }

	bool Reserve( uint32_t n )
	{
		if( n <= m_cap ) return true;
		uint32_t cap = m_cap ? m_cap * 2 : 8;
		if( cap < n ) cap = n;
		T* p = (T*)m_alloc->Alloc( sizeof(T) * cap, alignof(T) < 8 ? 8 : alignof(T) );
		if( !p ) return false;
		if( m_size ) memcpy( (void*)p, m_data, sizeof(T) * m_size );
		m_alloc->Free( m_data );
		m_data = p;
		m_cap  = cap;
		return true;
	}
	bool Resize( uint32_t n )
	{
		if( !Reserve( n ) ) return false;
		if( n > m_size ) memset( (void*)(m_data + m_size), 0, sizeof(T) * (n - m_size) );
		m_size = n;
		return true;
	}
	bool Push( const T& v )
	{
		if( m_size == m_cap && !Reserve( m_size + 1 ) ) return false;
		m_data[ m_size++ ] = v;
		return true;
	}
	void Pop() { --m_size; }
	void RemoveAt( uint32_t i )
	{
		memmove( (void*)(m_data + i), m_data + i + 1, sizeof(T) * (m_size - i - 1) );
		--m_size;
	}
	void Clear() { m_size = 0; }
	void Reset()
	{
		if( m_alloc ) m_alloc->Free( m_data );
		m_data = nullptr;
		m_size = m_cap = 0;
	}

	T&			operator[]( uint32_t i )		{ return m_data[i]; }
	const T&	operator[]( uint32_t i ) const	{ return m_data[i]; }
	T*			Data()							{ return m_data; }
	const T*	Data() const					{ return m_data; }
	uint32_t	Size() const					{ return m_size; }
	bool		Empty() const					{ return m_size == 0; }
	T&			Back()							{ return m_data[ m_size - 1 ]; }

private:
	const Allocator*	m_alloc	= nullptr;
	T*					m_data	= nullptr;
	uint32_t			m_size	= 0;
	uint32_t			m_cap	= 0;
};

//=========================================================================
// 文字列 → u32 のハッシュ表（キー文字列は呼び出し側が保持する）
//=========================================================================
inline uint32_t HashStr( const char* s )
{
	uint32_t h = 2166136261u;
	while( *s ){ h ^= (uint8_t)*s++; h *= 16777619u; }
	return h;
}

class StrMap {
public:
	StrMap() = default;
	StrMap( const StrMap& ) = delete;
	StrMap& operator=( const StrMap& ) = delete;
	~StrMap() { Reset(); }

	void Init( const Allocator* a ) { m_alloc = a; }
	void Reset()
	{
		if( m_alloc ){ m_alloc->Free( m_keys ); m_alloc->Free( m_vals ); }
		m_keys = nullptr;
		m_vals = nullptr;
		m_cap = m_count = 0;
	}

	bool Insert( const char* key, uint32_t value )
	{
		if( (m_count + 1) * 4 >= m_cap * 3 && !Grow() ) return false;
		InsertNoGrow( key, value );
		return true;
	}
	bool Find( const char* key, uint32_t* out ) const
	{
		if( !m_cap ) return false;
		uint32_t mask = m_cap - 1;
		for( uint32_t i = HashStr( key ) & mask;; i = (i + 1) & mask ){
			const char* k = m_keys[i];
			if( !k ) return false;
			if( strcmp( k, key ) == 0 ){ *out = m_vals[i]; return true; }
		}
	}

private:
	void InsertNoGrow( const char* key, uint32_t value )
	{
		uint32_t mask = m_cap - 1;
		for( uint32_t i = HashStr( key ) & mask;; i = (i + 1) & mask ){
			if( !m_keys[i] ){ m_keys[i] = key; m_vals[i] = value; ++m_count; return; }
			if( strcmp( m_keys[i], key ) == 0 ){ m_vals[i] = value; return; }
		}
	}
	bool Grow()
	{
		uint32_t n = m_cap ? m_cap * 2 : 16;
		const char** keys = (const char**)m_alloc->Alloc( sizeof(char*) * n, alignof(char*) );
		uint32_t*    vals = (uint32_t*)m_alloc->Alloc( sizeof(uint32_t) * n, alignof(uint32_t) );
		if( !keys || !vals ){ m_alloc->Free( keys ); m_alloc->Free( vals ); return false; }
		memset( (void*)keys, 0, sizeof(char*) * n );

		const char** oldKeys = m_keys;
		uint32_t*    oldVals = m_vals;
		uint32_t     oldCap  = m_cap;
		m_keys  = keys;
		m_vals  = vals;
		m_cap   = n;
		m_count = 0;
		for( uint32_t i = 0; i < oldCap; ++i ) if( oldKeys[i] ) InsertNoGrow( oldKeys[i], oldVals[i] );
		m_alloc->Free( (void*)oldKeys );
		m_alloc->Free( oldVals );
		return true;
	}

	const Allocator*	m_alloc	= nullptr;
	const char**		m_keys	= nullptr;
	uint32_t*			m_vals	= nullptr;
	uint32_t			m_cap	= 0;
	uint32_t			m_count	= 0;
};

//=========================================================================
// 書式付き文字列（vsnprintf）
//=========================================================================
void FormatV( char* buf, size_t size, const char* fmt, va_list ap );
void Format( char* buf, size_t size, const char* fmt, ... );

constexpr size_t kErrorSize = 512;

//=========================================================================
// 値の変換
//=========================================================================
inline bool IsIntLike( uint32_t t ) { return t == ATS_TYPE_INT || t == ATS_TYPE_ENUM; }
inline bool IsValidType( uint32_t t ) { return t >= ATS_TYPE_BOOL && t <= ATS_TYPE_HANDLE; }

inline int32_t	SlotI( uint64_t s ) { return (int32_t)(uint32_t)s; }
inline uint64_t	FromI( int32_t v ) { return (uint64_t)(uint32_t)v; }
inline float	SlotF( uint64_t s ) { uint32_t u = (uint32_t)s; float f; memcpy( &f, &u, 4 ); return f; }
inline uint64_t	FromF( float f ) { uint32_t u; memcpy( &u, &f, 4 ); return u; }

//=========================================================================
// ランタイム
//=========================================================================
struct CommandDef {
	char*			name;
	uint32_t		sig_hash;
	uint32_t		channel;	// kNone なら直列化しない
	ats_command_fn	fn;
	ats_cancel_fn	cancel;
	void*			user;
};

struct QueryDef {
	char*			name;
	uint32_t		sig_hash;
	ats_query_fn	fn;
	void*			user;
};

struct VarDef {
	uint32_t	id;
	char*		name;
	uint32_t	type;
	uint32_t	scope;
	uint64_t	init;		// STRING 以外の初期値
	char*		init_str;	// STRING の初期値
};

}	// namespace ats

struct ats_runtime {
	ats::Allocator				alloc;
	ats_log_fn					log;
	void*						log_user;
	ats::Array<ats::CommandDef>	commands;
	ats::Array<ats::QueryDef>	queries;
	ats::Array<ats::VarDef>		vars;
	ats::Array<char*>			channels;
	ats::StrMap					command_map;
	ats::StrMap					query_map;
	ats::StrMap					channel_map;
	char						error[ ats::kErrorSize ];

	void Log( ats_log_level level, const char* fmt, ... );
	int  FindVar( uint32_t id ) const;
};

//=========================================================================
// プログラム
//=========================================================================
namespace ats {

struct ResolvedImport {
	uint8_t		kind;		// fmt::ImportKind
	uint32_t	index;		// commands / queries のインデックス
};

}	// namespace ats

struct ats_program {
	ats_runtime*						rt;
	uint32_t							refcount;
	uint8_t*							bytes;
	uint32_t							size;

	const char*							script_id;
	ats::Array<const char*>				strings;		// bytes 内を指す
	ats::Array<uint64_t>				consts;
	ats::Array<ats::fmt::ImportEntry>	imports;
	ats::Array<ats::ResolvedImport>		resolved;		// imports と同じ並び
	ats::Array<ats::fmt::VarEntry>		vars;
	ats::Array<int32_t>					var_shared;		// 共有変数なら rt->vars のインデックス、それ以外 -1
	ats::Array<ats::fmt::EntryEntry>	entries;
	ats::Array<ats::fmt::DebugEntry>	debug;
	const uint8_t*						code;			// bytes 内を指す
	uint32_t							code_size;

	int  FindEntry( const char* name, uint8_t kind ) const;
	uint32_t LineOf( uint32_t pc ) const;
};

//=========================================================================
// VM
//=========================================================================
namespace ats {

// スクリプト変数
struct ScriptVar {
	char*		name;
	char*		was_name;	// @was の旧名（なければ nullptr）
	uint32_t	type;
	uint32_t	flags;
	uint64_t	init;
	uint64_t	value;
};

// まだ読み込まれていないスクリプトのセーブ値
struct DormantVar {
	char*		name;
	uint32_t	type;
	uint64_t	value;		// STRING 以外
	char*		str;		// STRING の値
};

struct ScriptBlock {
	char*				script_id;
	Array<ScriptVar>	vars;
	Array<DormantVar>	dormant;
};

// プログラムと VM の結び付き
struct VarRef {
	uint8_t		script;		// 0: 共有変数, 1: スクリプト変数
	uint32_t	index;
};

struct Binding {
	ats_program*		prog;
	ScriptBlock*		block;
	Array<VarRef>		vars;
	Array<uint32_t>		str_ids;	// プログラムの文字列インデックス → VM の文字列 ID
};

enum class FiberState : uint8_t {
	Free,
	Ready,
	Sleeping,		// 秒数待ち
	WaitFrames,		// フレーム数待ち
	WaitCall,		// ATS_PENDING の完了待ち
	CallRunning,	// ATS_RUNNING の再呼び出し待ち
	WaitChannel,	// チャンネルの空き待ち
	WaitJoin,		// JOIN / RACE で子ファイバ待ち
	Dying,			// 終了処理中
};

struct Frame {
	uint32_t	entry;
	uint32_t	ret_pc;
	uint32_t	locals_base;
};

}	// namespace ats

struct ats_call {
	ats_vm*					vm;
	uint32_t				fiber_index;
	uint32_t				import;			// 呼び出し中のインポート
	uint32_t				token_seq;		// 0 ならトークン未発行
	uint32_t				retry;
	bool					active;
	bool					has_result;
	bool					holds_channel;
	uint8_t					outcome;		// ハンドラ実行中に完了通知が来た（1: 完了, 2: 失敗）
	ats_value				result;
	ats::Array<ats_value>	args;
};

namespace ats {

struct Fiber {
	FiberState		state;
	uint32_t		generation;
	uint32_t		index;
	Binding*		binding;
	uint32_t		pc;
	uint32_t		parent;			// 親ファイバの index + 1（0 ならなし）
	uint32_t		parent_gen;
	uint32_t		live_children;
	uint32_t		finished_children;
	uint8_t			join_mode;		// OP_JOIN / OP_RACE
	bool			abort_requested;
	float			sleep;
	uint32_t		frames;
	Array<uint64_t>	stack;
	Array<Frame>	callstack;
	Array<uint32_t>	children;		// 子ファイバの index
	ats_call		call;
};

struct Channel {
	uint32_t		owner;			// ファイバ index + 1（0 なら空き）
	Array<uint64_t>	waiters;		// ats_fiber_id
};

struct InternStr {
	char*		str;
};

}	// namespace ats

struct ats_vm {
	ats_runtime*					rt;
	const ats::Allocator*			alloc;
	uint32_t						budget;
	uint32_t						max_stack;
	uint32_t						max_depth;
	uint64_t						rng;
	bool							in_update;
	uint32_t						current;		// 実行中のファイバ index + 1
	uint32_t						token_seq;

	ats::Array<uint64_t>			shared;			// rt->vars と同じ並び
	ats::Array<ats::ScriptBlock*>	blocks;
	ats::Array<ats::Binding*>		bindings;
	ats::Array<ats::Fiber*>			fibers;
	ats::Array<ats::Channel*>		channels;		// rt->channels と同じ並び
	ats::Array<char*>				interned;		// 文字列 ID → 文字列
	ats::StrMap						intern_map;
	char							error[ ats::kErrorSize ];

	// 文字列
	uint32_t	Intern( const char* s );
	const char*	Str( uint64_t id ) const { return (uint32_t)id < interned.Size() ? interned[ (uint32_t)id ] : ""; }

	// 値の変換
	uint64_t	ToSlot( const ats_value& v );
	ats_value	FromSlot( uint64_t s, uint32_t type ) const;
	bool		ConvertSlot( uint64_t* slot, uint32_t from, uint32_t to, const char* fromStr );

	// 結び付け
	ats::Binding*		FindBinding( ats_program* prog );
	ats_result			Attach( ats_program* prog, ats::Binding** out );
	ats::ScriptBlock*	FindBlock( const char* script_id );
	ats::ScriptBlock*	GetBlock( const char* script_id );

	// ファイバ
	ats::Fiber*		NewFiber( ats::Binding* b, uint32_t entry );
	ats::Fiber*		GetFiber( ats_fiber_id id );
	ats_fiber_id	FiberId( const ats::Fiber* f ) const;
	void			AbortFiber( ats::Fiber* f, bool cancel );
	void			FinishFiber( ats::Fiber* f );
	void			FiberError( ats::Fiber* f, const char* fmt, ... );
	void			ReleaseChannel( ats::Fiber* f );
	ats_result		StartEvent( ats::Binding* b, const char* event, const ats_value* args, int argc, ats_fiber_id* out );
	void			RunFiber( ats::Fiber* f, uint32_t& remaining );
	bool			ResumeRunningCall( ats::Fiber* f );

	void			Log( ats_log_level level, const char* fmt, ... );

	// スクリプト変数を初期値に戻す（セーブ済みで未読み込みの値も捨てる）
	void			ResetBlock( ats::ScriptBlock* blk );
};

#endif	// ATS_INTERNAL_H
