/**************************************************************************/
/*!	\file	ats_writer.cpp
	\brief	.atsb の書き出し
***************************************************************************/
#include "ats_writer.h"

#include <cstring>
#include <stdexcept>

namespace ats {
namespace writer {

using namespace fmt;

namespace {

template<class T> T Rd( const uint8_t* p ) { T v; std::memcpy( &v, p, sizeof(T) ); return v; }

template<class T> void Append( std::vector<uint8_t>& out, const T& v )
{
	const uint8_t* p = reinterpret_cast<const uint8_t*>( &v );
	out.insert( out.end(), p, p + sizeof(T) );
}

void FillTypes( uint8_t* dst, const std::vector<ats_type>& types )
{
	std::memset( dst, 0, kMaxParams );
	for( size_t i = 0; i < types.size() && i < (size_t)kMaxParams; ++i ) dst[i] = (uint8_t)types[i];
}

}	// namespace

//=========================================================================
// テーブル
//=========================================================================
ProgramWriter::ProgramWriter( const std::string& script_id )
	: m_scriptId( script_id )
{
	Str( script_id );
}

uint32_t ProgramWriter::Str( const std::string& s )
{
	for( size_t i = 0; i < m_strings.size(); ++i ) if( m_strings[i] == s ) return (uint32_t)i;
	m_strings.push_back( s );
	return (uint32_t)m_strings.size() - 1;
}

uint32_t ProgramWriter::Const( uint64_t v )
{
	for( size_t i = 0; i < m_consts.size(); ++i ) if( m_consts[i] == v ) return (uint32_t)i;
	m_consts.push_back( v );
	return (uint32_t)m_consts.size() - 1;
}

uint16_t ProgramWriter::Command( const std::string& name, const std::vector<ats_type>& params, ats_type ret, uint32_t sig_hash )
{
	ImportEntry e;
	std::memset( &e, 0, sizeof(e) );
	e.name     = Str( name );
	e.kind     = kImportCommand;
	e.argc     = (uint8_t)params.size();
	e.retc     = ret != ATS_TYPE_VOID ? 1 : 0;
	e.sig_hash = sig_hash;
	e.ret_type = (uint8_t)ret;
	FillTypes( e.param_types, params );
	m_imports.push_back( e );
	return (uint16_t)(m_imports.size() - 1);
}

uint16_t ProgramWriter::Query( const std::string& name, const std::vector<ats_type>& params, ats_type ret, uint32_t sig_hash )
{
	uint16_t i = Command( name, params, ret, sig_hash );
	m_imports[i].kind = kImportQuery;
	return i;
}

uint16_t ProgramWriter::SharedVar( uint32_t id, ats_type type )
{
	VarEntry v;
	std::memset( &v, 0, sizeof(v) );
	v.kind     = kVarShared;
	v.type     = (uint8_t)type;
	v.id       = id;
	v.name     = kNone;
	v.was_name = kNone;
	m_vars.push_back( v );
	return (uint16_t)(m_vars.size() - 1);
}

uint16_t ProgramWriter::ScriptVar( const std::string& name, ats_type type, uint64_t init, bool transient, const std::string& was )
{
	VarEntry v;
	std::memset( &v, 0, sizeof(v) );
	v.kind     = kVarScript;
	v.type     = (uint8_t)type;
	v.flags    = transient ? kVarTransient : 0;
	v.name     = Str( name );
	v.was_name = was.empty() ? kNone : Str( was );
	v.init     = init;
	m_vars.push_back( v );
	return (uint16_t)(m_vars.size() - 1);
}

uint16_t ProgramWriter::ScriptVarStr( const std::string& name, const std::string& init, bool transient, const std::string& was )
{
	return ScriptVar( name, ATS_TYPE_STRING, Str( init ), transient, was );
}

//=========================================================================
// エントリ
//=========================================================================
uint16_t ProgramWriter::DeclareFunction( const std::string& name, const std::vector<ats_type>& params, ats_type ret, uint16_t localc )
{
	Entry en;
	std::memset( &en.e, 0, sizeof(en.e) );
	en.e.name     = Str( name );
	en.e.kind     = kEntryFunction;
	en.e.paramc   = (uint8_t)params.size();
	en.e.retc     = ret != ATS_TYPE_VOID ? 1 : 0;
	en.e.ret_type = (uint8_t)ret;
	en.e.localc   = localc < params.size() ? (uint16_t)params.size() : localc;
	FillTypes( en.e.param_types, params );
	en.defined = false;
	m_entries.push_back( en );
	return (uint16_t)(m_entries.size() - 1);
}

uint16_t ProgramWriter::BeginEvent( const std::string& name, const std::vector<ats_type>& params, uint16_t localc )
{
	uint16_t i = DeclareFunction( name, params, ATS_TYPE_VOID, localc );
	m_entries[i].e.kind = kEntryEvent;
	m_entries[i].e.code = CodePos();
	m_entries[i].defined = true;
	m_current = i;
	return i;
}

void ProgramWriter::BeginFunction( uint16_t fn )
{
	m_entries[fn].e.code   = CodePos();
	m_entries[fn].defined = true;
	m_current = fn;
}

void ProgramWriter::EndEntry()
{
	m_current = -1;
}

//=========================================================================
// 命令
//=========================================================================
void ProgramWriter::Put( const void* p, size_t n )
{
	const uint8_t* b = static_cast<const uint8_t*>( p );
	m_code.insert( m_code.end(), b, b + n );
}

void ProgramWriter::Raw( const std::vector<uint8_t>& bytes )	{ m_code.insert( m_code.end(), bytes.begin(), bytes.end() ); }
void ProgramWriter::Op( fmt::Op op )							{ U8( op ); }
void ProgramWriter::PushI( int32_t v )							{ U8( OP_PUSH_I32 ); Put( &v, 4 ); }
void ProgramWriter::PushF( float v )							{ U8( OP_PUSH_F32 ); Put( &v, 4 ); }
void ProgramWriter::PushK( uint64_t v )							{ U8( OP_PUSH_K ); U32( Const( v ) ); }
void ProgramWriter::PushStr( const std::string& s )				{ U8( OP_PUSH_STR ); U32( Str( s ) ); }
void ProgramWriter::LdLocal( uint16_t i )						{ U8( OP_LD_LOCAL ); U16( i ); }
void ProgramWriter::StLocal( uint16_t i )						{ U8( OP_ST_LOCAL ); U16( i ); }
void ProgramWriter::LdVar( uint16_t i )							{ U8( OP_LD_VAR ); U16( i ); }
void ProgramWriter::StVar( uint16_t i )							{ U8( OP_ST_VAR ); U16( i ); }
void ProgramWriter::CallCmd( uint16_t imp )						{ U8( OP_CALL_CMD ); U16( imp ); U8( m_imports[imp].argc ); }
void ProgramWriter::CallQuery( uint16_t imp )					{ U8( OP_CALL_QUERY ); U16( imp ); U8( m_imports[imp].argc ); }
void ProgramWriter::CallFn( uint16_t fn )						{ U8( OP_CALL_FN ); U16( fn ); }
void ProgramWriter::Fire( const std::string& event, uint8_t argc ) { U8( OP_FIRE ); U32( Str( event ) ); U8( argc ); }

Label ProgramWriter::NewLabel()
{
	m_labels.push_back( -1 );
	Label l;
	l.id = (int)m_labels.size() - 1;
	return l;
}

void ProgramWriter::Bind( Label l )
{
	m_labels[ (size_t)l.id ] = CodePos();
}

void ProgramWriter::JumpOp( fmt::Op op, Label l )
{
	U8( op );
	Fixup fx;
	fx.pos   = CodePos();
	fx.base  = CodePos() + 4;
	fx.label = l.id;
	m_fixups.push_back( fx );
	U32( 0 );
}

void ProgramWriter::Jmp( Label l )	{ JumpOp( OP_JMP, l ); }
void ProgramWriter::Jz( Label l )	{ JumpOp( OP_JZ, l ); }
void ProgramWriter::Jnz( Label l )	{ JumpOp( OP_JNZ, l ); }
void ProgramWriter::Fork( Label l )	{ JumpOp( OP_FORK, l ); }

void ProgramWriter::Switch( const std::vector<std::pair<int32_t, Label>>& cases, Label def )
{
	U8( OP_SWITCH );
	U16( (uint16_t)cases.size() );
	const uint32_t base = CodePos() + 4 + (uint32_t)cases.size() * 8;
	m_fixups.push_back( { CodePos(), base, def.id } );
	U32( 0 );
	for( const auto& c : cases ){
		U32( (uint32_t)c.first );
		m_fixups.push_back( { CodePos(), base, c.second.id } );
		U32( 0 );
	}
}

void ProgramWriter::Line( uint32_t line, const std::string& node )
{
	DebugEntry d;
	d.pc   = CodePos();
	d.line = line;
	d.node = node.empty() ? kNone : Str( node );
	m_debug.push_back( d );
}

//=========================================================================
// max_stack の計算（VM の検証器と同じ規則）
//=========================================================================
bool ProgramWriter::ComputeMaxStack( EntryEntry& e, std::string* error ) const
{
	const uint32_t size = (uint32_t)m_code.size();
	std::vector<int32_t> depth( size, -1 );
	std::vector<uint32_t> work;
	int32_t maxDepth = 0;

	auto fail = [&]( const std::string& msg ) { if( error ) *error = msg; return false; };
	auto push = [&]( uint32_t pc, int32_t d ) -> bool {
		if( pc >= size || d < 0 ) return false;
		if( depth[pc] < 0 ){ depth[pc] = d; work.push_back( pc ); }
		else if( depth[pc] != d ) return false;
		if( d > maxDepth ) maxDepth = d;
		return true;
	};

	if( !push( e.code, 0 ) ) return fail( "entry code out of range" );
	while( !work.empty() ){
		uint32_t pc = work.back();
		work.pop_back();
		int32_t d = depth[pc];
		uint8_t op = m_code[pc];
		const uint8_t* a = m_code.data() + pc + 1;
		uint32_t next = pc + 1 + (uint32_t)OperandSize( op );
		if( op == OP_SWITCH ) next += (uint32_t)Rd<uint16_t>( a ) * 8;

		int32_t pops = 0, pushes = 0;
		bool fall = true;
		switch( op ){
		case OP_PUSH_I32: case OP_PUSH_F32: case OP_PUSH_K: case OP_PUSH_STR:
		case OP_LD_LOCAL: case OP_LD_VAR:
			pushes = 1; break;
		case OP_POP: case OP_ST_LOCAL: case OP_ST_VAR: case OP_SLEEP: case OP_WAIT_FRAMES:
			pops = 1; break;
		case OP_DUP: pops = 1; pushes = 2; break;
		case OP_NEG_I: case OP_NEG_F: case OP_BNOT: case OP_NOT: case OP_I2F: case OP_F2I: case OP_RAND:
			pops = 1; pushes = 1; break;
		case OP_JMP:
			fall = false;
			if( !push( next + (uint32_t)Rd<int32_t>( a ), d ) ) return fail( "bad jump" );
			break;
		case OP_JZ: case OP_JNZ:
			pops = 1;
			if( !push( next + (uint32_t)Rd<int32_t>( a ), d - 1 ) ) return fail( "bad jump" );
			break;
		case OP_SWITCH: {
			pops = 1; fall = false;
			uint16_t count = Rd<uint16_t>( a );
			if( !push( next + (uint32_t)Rd<int32_t>( a + 2 ), d - 1 ) ) return fail( "bad switch" );
			for( uint32_t k = 0; k < count; ++k )
				if( !push( next + (uint32_t)Rd<int32_t>( a + 6 + k * 8 + 4 ), d - 1 ) ) return fail( "bad switch" );
			break;
		}
		case OP_CALL_CMD: case OP_CALL_QUERY: {
			const ImportEntry& ie = m_imports[ Rd<uint16_t>( a ) ];
			pops = ie.argc; pushes = ie.retc;
			break;
		}
		case OP_CALL_FN: {
			const EntryEntry& fe = m_entries[ Rd<uint16_t>( a ) ].e;
			pops = fe.paramc; pushes = fe.retc;
			break;
		}
		case OP_RET: case OP_END: fall = false; break;
		case OP_FORK:
			if( !push( next + (uint32_t)Rd<int32_t>( a ), 0 ) ) return fail( "bad fork" );
			break;
		case OP_FIRE: pops = a[4]; break;
		default:
			if( op >= OP_ADD_I && op <= OP_NE_H && op != OP_NEG_I && op != OP_NEG_F && op != OP_BNOT && op != OP_NOT ){
				pops = 2; pushes = 1;
			}
			break;
		}
		if( d < pops ) return fail( "stack underflow at " + std::to_string( pc ) );
		if( fall && !push( next, d - pops + pushes ) ) return fail( "inconsistent stack at " + std::to_string( next ) );
	}
	e.max_stack = (uint16_t)maxDepth;
	return true;
}

//=========================================================================
// 書き出し
//=========================================================================
std::vector<uint8_t> ProgramWriter::Build( std::string* error )
{
	// ラベルを解決する
	for( const Fixup& fx : m_fixups ){
		int64_t target = m_labels[ (size_t)fx.label ];
		if( target < 0 ){ if( error ) *error = "unbound label"; return {}; }
		int32_t rel = (int32_t)(target - (int64_t)fx.base);
		std::memcpy( &m_code[fx.pos], &rel, 4 );
	}
	m_fixups.clear();

	std::vector<EntryEntry> entries;
	for( Entry& en : m_entries ){
		if( !en.defined ){ if( error ) *error = "function '" + m_strings[en.e.name] + "' has no body"; return {}; }
		if( m_fixedMaxStack >= 0 ) en.e.max_stack = (uint16_t)m_fixedMaxStack;
		else if( !ComputeMaxStack( en.e, error ) ) return {};
		entries.push_back( en.e );
	}

	// セクションの中身
	std::vector<uint8_t> strs;
	{
		Append( strs, (uint32_t)m_strings.size() );
		uint32_t off = 4 + (uint32_t)m_strings.size() * 4;
		for( const std::string& s : m_strings ){ Append( strs, off ); off += (uint32_t)s.size() + 1; }
		for( const std::string& s : m_strings ){ strs.insert( strs.end(), s.begin(), s.end() ); strs.push_back( 0 ); }
	}
	auto table = [&]( const auto& v ){
		std::vector<uint8_t> out;
		Append( out, (uint32_t)v.size() );
		for( const auto& x : v ) Append( out, x );
		return out;
	};
	std::vector<uint8_t> cnst = table( m_consts );
	std::vector<uint8_t> impt = table( m_imports );
	std::vector<uint8_t> vars = table( m_vars );
	std::vector<uint8_t> entr = table( entries );
	std::vector<uint8_t> dbug = table( m_debug );

	struct Sec { uint32_t tag; const std::vector<uint8_t>* data; };
	const Sec secs[] = {
		{ kSecStrings, &strs }, { kSecConsts, &cnst }, { kSecImports, &impt }, { kSecVars, &vars },
		{ kSecEntries, &entr }, { kSecCode, &m_code }, { kSecDebug, &dbug },
	};
	const uint32_t count = (uint32_t)(sizeof(secs) / sizeof(secs[0]));

	std::vector<uint8_t> out( sizeof(FileHeader) + count * sizeof(SectionEntry), 0 );
	std::vector<SectionEntry> table2;
	for( const Sec& s : secs ){
		while( out.size() % 8 ) out.push_back( 0 );
		SectionEntry se;
		se.tag      = s.tag;
		se.offset   = (uint32_t)out.size();
		se.size     = (uint32_t)s.data->size();
		se.reserved = 0;
		table2.push_back( se );
		out.insert( out.end(), s.data->begin(), s.data->end() );
	}

	FileHeader h;
	h.magic         = kMagic;
	h.version_major = kVersionMajor;
	h.version_minor = kVersionMinor;
	h.flags         = 0;
	h.file_size     = (uint32_t)out.size();
	h.manifest_hash = m_manifestHash;
	h.script_id     = 0;
	h.section_count = count;
	std::memcpy( out.data(), &h, sizeof(h) );
	std::memcpy( out.data() + sizeof(h), table2.data(), sizeof(SectionEntry) * count );
	return out;
}

}	// namespace writer
}	// namespace ats
