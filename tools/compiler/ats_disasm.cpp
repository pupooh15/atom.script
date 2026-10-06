/**************************************************************************/
/*!	\file	ats_disasm.cpp
	\brief	.atsb の逆アセンブル（atsc disasm）
***************************************************************************/
#include "ats_compiler.h"
#include "atomscript/ats_format.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>

namespace ats {
namespace compiler {

using namespace ats::fmt;

namespace {

template<class T> T Rd( const uint8_t* p ) { T v; std::memcpy( &v, p, sizeof(T) ); return v; }

const char* OpName( uint8_t op )
{
	static const char* const names[] = {
		"NOP", "PUSH_I32", "PUSH_F32", "PUSH_K", "PUSH_STR", "POP", "DUP",
		"LD_LOCAL", "ST_LOCAL", "LD_VAR", "ST_VAR",
		"ADD_I", "SUB_I", "MUL_I", "DIV_I", "MOD_I", "NEG_I",
		"ADD_F", "SUB_F", "MUL_F", "DIV_F", "NEG_F",
		"BAND", "BOR", "BXOR", "SHL", "SHR", "BNOT", "NOT",
		"EQ_I", "NE_I", "LT_I", "LE_I", "GT_I", "GE_I",
		"EQ_F", "NE_F", "LT_F", "LE_F", "GT_F", "GE_F",
		"EQ_H", "NE_H", "I2F", "F2I",
		"JMP", "JZ", "JNZ", "SWITCH",
		"CALL_CMD", "CALL_QUERY", "CALL_FN", "RET",
		"FORK", "JOIN", "RACE", "YIELD", "SLEEP", "WAIT_FRAMES", "END",
		"FIRE", "RESET_VARS", "RAND",
	};
	static_assert( sizeof(names) / sizeof(names[0]) == OP_COUNT, "op names" );
	return op < OP_COUNT ? names[op] : "???";
}

const char* TypeName( uint8_t t )
{
	switch( t ){
	case ATS_TYPE_VOID:		return "void";
	case ATS_TYPE_BOOL:		return "bool";
	case ATS_TYPE_INT:		return "int";
	case ATS_TYPE_FLOAT:	return "float";
	case ATS_TYPE_STRING:	return "string";
	case ATS_TYPE_ENUM:		return "enum";
	case ATS_TYPE_HANDLE:	return "handle";
	}
	return "?";
}

std::string Fmt( const char* f, ... )
{
	char buf[512];
	va_list ap;
	va_start( ap, f );
	vsnprintf( buf, sizeof(buf), f, ap );
	va_end( ap );
	return buf;
}

}	// namespace

bool Disassemble( const std::vector<uint8_t>& b, std::string* out, std::string* error )
{
	auto fail = [&]( const char* msg ) { if( error ) *error = msg; return false; };
	if( b.size() < sizeof(FileHeader) ) return fail( "ファイルが小さすぎます" );
	FileHeader h = Rd<FileHeader>( b.data() );
	if( h.magic != kMagic ) return fail( ".atsb ではありません" );
	if( h.file_size > b.size() || sizeof(FileHeader) + (size_t)h.section_count * sizeof(SectionEntry) > b.size() )
		return fail( "ファイルが壊れています" );

	std::map<uint32_t, std::pair<const uint8_t*, uint32_t>> sec;
	for( uint32_t i = 0; i < h.section_count; ++i ){
		SectionEntry s = Rd<SectionEntry>( b.data() + sizeof(FileHeader) + i * sizeof(SectionEntry) );
		if( (uint64_t)s.offset + s.size > b.size() ) return fail( "セクションが範囲外です" );
		sec[s.tag] = { b.data() + s.offset, s.size };
	}

	// 文字列
	std::vector<std::string> strs;
	if( sec.count( kSecStrings ) ){
		auto s = sec[kSecStrings];
		if( s.second < 4 ) return fail( "STRS が壊れています" );
		uint32_t n = Rd<uint32_t>( s.first );
		if( n > (s.second - 4) / 4 ) return fail( "STRS が壊れています" );
		for( uint32_t i = 0; i < n; ++i ){
			uint32_t off = Rd<uint32_t>( s.first + 4 + i * 4 );
			if( off >= s.second || !std::memchr( s.first + off, 0, s.second - off ) ) return fail( "STRS が壊れています" );
			strs.push_back( (const char*)(s.first + off) );
		}
	}
	auto S = [&]( uint32_t i ) -> std::string { return i < strs.size() ? strs[i] : (i == kNone ? "" : "<?>"); };

	auto table = [&]( uint32_t tag, size_t elem, uint32_t* count ) -> const uint8_t* {
		*count = 0;
		if( !sec.count( tag ) ) return nullptr;
		auto s = sec[tag];
		if( s.second < 4 ) return nullptr;
		uint32_t n = Rd<uint32_t>( s.first );
		if( n > (s.second - 4) / elem ) return nullptr;
		*count = n;
		return s.first + 4;
	};

	std::string o;
	o += Fmt( "script \"%s\"   format %u.%u   manifest 0x%016llx\n", S( h.script_id ).c_str(),
			  h.version_major, h.version_minor, (unsigned long long)h.manifest_hash );

	uint32_t nc, ni, nv, ne, nd;
	const uint8_t* cst = table( kSecConsts, 8, &nc );
	const uint8_t* imp = table( kSecImports, sizeof(ImportEntry), &ni );
	const uint8_t* var = table( kSecVars, sizeof(VarEntry), &nv );
	const uint8_t* ent = table( kSecEntries, sizeof(EntryEntry), &ne );
	const uint8_t* dbg = table( kSecDebug, sizeof(DebugEntry), &nd );

	o += "\n[imports]\n";
	std::vector<ImportEntry> imports;
	for( uint32_t i = 0; i < ni; ++i ){
		ImportEntry e = Rd<ImportEntry>( imp + i * sizeof(ImportEntry) );
		imports.push_back( e );
		std::string params;
		for( int k = 0; k < e.argc && k < kMaxParams; ++k ) params += std::string( k ? ", " : "" ) + TypeName( e.param_types[k] );
		o += Fmt( "  %3u  %-7s %s(%s) -> %s   sig=0x%08x\n", i, e.kind == kImportQuery ? "query" : "command",
				  S( e.name ).c_str(), params.c_str(), e.retc ? TypeName( e.ret_type ) : "void", e.sig_hash );
	}

	o += "\n[vars]\n";
	for( uint32_t i = 0; i < nv; ++i ){
		VarEntry v = Rd<VarEntry>( var + i * sizeof(VarEntry) );
		if( v.kind == kVarShared ){
			o += Fmt( "  %3u  shared  id=%u  %s\n", i, v.id, TypeName( v.type ) );
		} else {
			std::string init = v.type == ATS_TYPE_STRING ? "\"" + S( (uint32_t)v.init ) + "\"" : std::to_string( (int32_t)(uint32_t)v.init );
			o += Fmt( "  %3u  script  %s: %s = %s%s%s\n", i, S( v.name ).c_str(), TypeName( v.type ), init.c_str(),
					  (v.flags & kVarTransient) ? "  transient" : "",
					  v.was_name != kNone ? ("  @was(\"" + S( v.was_name ) + "\")").c_str() : "" );
		}
	}

	o += "\n[entries]\n";
	std::vector<EntryEntry> entries;
	std::map<uint32_t, std::string> labels;
	for( uint32_t i = 0; i < ne; ++i ){
		EntryEntry e = Rd<EntryEntry>( ent + i * sizeof(EntryEntry) );
		entries.push_back( e );
		std::string params;
		for( int k = 0; k < e.paramc && k < kMaxParams; ++k ) params += std::string( k ? ", " : "" ) + TypeName( e.param_types[k] );
		o += Fmt( "  %3u  %-5s %s(%s)%s   locals=%u max_stack=%u code=%04x\n", i, e.kind == kEntryEvent ? "event" : "fn",
				  S( e.name ).c_str(), params.c_str(), e.retc ? (std::string( " -> " ) + TypeName( e.ret_type )).c_str() : "",
				  e.localc, e.max_stack, e.code );
		labels[e.code] = S( e.name );
	}

	std::map<uint32_t, DebugEntry> lines;
	for( uint32_t i = 0; i < nd; ++i ){
		DebugEntry d = Rd<DebugEntry>( dbg + i * sizeof(DebugEntry) );
		lines[d.pc] = d;
	}

	o += "\n[code]\n";
	if( sec.count( kSecCode ) ){
		const uint8_t* code = sec[kSecCode].first;
		uint32_t size = sec[kSecCode].second;
		uint32_t pc = 0;
		while( pc < size ){
			auto lb = labels.find( pc );
			if( lb != labels.end() ) o += Fmt( "%s:\n", lb->second.c_str() );
			uint8_t op = code[pc];
			uint32_t len = 1 + (uint32_t)OperandSize( op );
			if( op == OP_SWITCH && pc + 3 <= size ) len += (uint32_t)Rd<uint16_t>( code + pc + 1 ) * 8;
			if( op >= OP_COUNT || pc + len > size ){ o += Fmt( "  %04x  ???\n", pc ); break; }
			const uint8_t* a = code + pc + 1;
			uint32_t next = pc + len;
			std::string arg;
			switch( op ){
			case OP_PUSH_I32:	arg = std::to_string( Rd<int32_t>( a ) ); break;
			case OP_PUSH_F32:	arg = Fmt( "%g", Rd<float>( a ) ); break;
			case OP_PUSH_K: {
				uint32_t k = Rd<uint32_t>( a );
				arg = k < nc ? Fmt( "0x%llx", (unsigned long long)Rd<uint64_t>( cst + k * 8 ) ) : "<?>";
				break;
			}
			case OP_PUSH_STR:	arg = "\"" + S( Rd<uint32_t>( a ) ) + "\""; break;
			case OP_LD_LOCAL: case OP_ST_LOCAL: case OP_LD_VAR: case OP_ST_VAR:
				arg = std::to_string( Rd<uint16_t>( a ) ); break;
			case OP_JMP: case OP_JZ: case OP_JNZ: case OP_FORK:
				arg = Fmt( "%04x", next + (uint32_t)Rd<int32_t>( a ) ); break;
			case OP_SWITCH: {
				uint16_t n = Rd<uint16_t>( a );
				arg = Fmt( "default=%04x", next + (uint32_t)Rd<int32_t>( a + 2 ) );
				for( uint32_t k = 0; k < n; ++k )
					arg += Fmt( "  %d=>%04x", Rd<int32_t>( a + 6 + k * 8 ), next + (uint32_t)Rd<int32_t>( a + 6 + k * 8 + 4 ) );
				break;
			}
			case OP_CALL_CMD: case OP_CALL_QUERY: {
				uint16_t i = Rd<uint16_t>( a );
				arg = (i < imports.size() ? S( imports[i].name ) : "<?>") + Fmt( " argc=%u", a[2] );
				break;
			}
			case OP_CALL_FN: {
				uint16_t i = Rd<uint16_t>( a );
				arg = i < entries.size() ? S( entries[i].name ) : "<?>";
				break;
			}
			case OP_FIRE:	arg = S( Rd<uint32_t>( a ) ) + Fmt( " argc=%u", a[4] ); break;
			default: break;
			}
			std::string comment;
			auto ln = lines.find( pc );
			if( ln != lines.end() ){
				comment = Fmt( "; line %u", ln->second.line );
				if( ln->second.node != kNone ) comment += " @node(\"" + S( ln->second.node ) + "\")";
			}
			o += Fmt( "  %04x  %-11s %-28s %s\n", pc, OpName( op ), arg.c_str(), comment.c_str() );
			pc = next;
		}
	}
	*out = o;
	return true;
}

}	// namespace compiler
}	// namespace ats
