/**************************************************************************/
/*!	\file	ats_json.cpp
	\brief	最小限の JSON
***************************************************************************/
#include "ats_json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace ats {
namespace json {

const Value& Value::Empty()
{
	static const Value v;
	return v;
}

const Value& Value::operator[]( const char* key ) const
{
	if( m_kind != Object ) return Empty();
	for( const auto& kv : m_obj ) if( kv.first == key ) return kv.second;
	return Empty();
}

bool Value::Has( const char* key ) const
{
	if( m_kind != Object ) return false;
	for( const auto& kv : m_obj ) if( kv.first == key ) return true;
	return false;
}

Value& Value::Set( const std::string& key, Value v )
{
	m_kind = Object;
	for( auto& kv : m_obj ) if( kv.first == key ){ kv.second = std::move( v ); return kv.second; }
	m_obj.emplace_back( key, std::move( v ) );
	return m_obj.back().second;
}

static void DumpString( const std::string& s, std::string& out )
{
	out += '"';
	for( unsigned char c : s ){
		switch( c ){
		case '"':	out += "\\\""; break;
		case '\\':	out += "\\\\"; break;
		case '\n':	out += "\\n"; break;
		case '\r':	out += "\\r"; break;
		case '\t':	out += "\\t"; break;
		default:
			if( c < 0x20 ){ char b[8]; snprintf( b, sizeof(b), "\\u%04x", c ); out += b; }
			else out += (char)c;
		}
	}
	out += '"';
}

void Value::DumpTo( std::string& out ) const
{
	switch( m_kind ){
	case Null:	out += "null"; break;
	case Bool:	out += m_bool ? "true" : "false"; break;
	case Number: {
		if( std::floor( m_num ) == m_num && std::fabs( m_num ) < 9.0e15 ){
			out += std::to_string( (long long)m_num );
		} else {
			char b[32];
			snprintf( b, sizeof(b), "%.17g", m_num );
			out += b;
		}
		break;
	}
	case String: DumpString( m_str, out ); break;
	case Array:
		out += '[';
		for( size_t i = 0; i < m_arr.size(); ++i ){ if( i ) out += ','; m_arr[i].DumpTo( out ); }
		out += ']';
		break;
	case Object:
		out += '{';
		for( size_t i = 0; i < m_obj.size(); ++i ){
			if( i ) out += ',';
			DumpString( m_obj[i].first, out );
			out += ':';
			m_obj[i].second.DumpTo( out );
		}
		out += '}';
		break;
	}
}

std::string Value::Dump() const
{
	std::string s;
	DumpTo( s );
	return s;
}

//=========================================================================
// 解析
//=========================================================================
namespace {

struct Reader {
	const std::string&	t;
	size_t				i = 0;
	int					depth = 0;

	explicit Reader( const std::string& s ) : t( s ) {}

	void Ws() { while( i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r') ) ++i; }

	static void PutUtf8( std::string& o, uint32_t cp )
	{
		if( cp < 0x80 ) o += (char)cp;
		else if( cp < 0x800 ){ o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
		else if( cp < 0x10000 ){ o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
		else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 0x3F)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
	}

	bool Hex4( uint32_t* out )
	{
		if( i + 4 > t.size() ) return false;
		uint32_t v = 0;
		for( int k = 0; k < 4; ++k ){
			char c = t[i++];
			v <<= 4;
			if( c >= '0' && c <= '9' ) v |= (uint32_t)(c - '0');
			else if( c >= 'a' && c <= 'f' ) v |= (uint32_t)(c - 'a' + 10);
			else if( c >= 'A' && c <= 'F' ) v |= (uint32_t)(c - 'A' + 10);
			else return false;
		}
		*out = v;
		return true;
	}

	bool Str( std::string* out )
	{
		if( t[i] != '"' ) return false;
		++i;
		while( i < t.size() ){
			char c = t[i++];
			if( c == '"' ) return true;
			if( c != '\\' ){ *out += c; continue; }
			if( i >= t.size() ) return false;
			char e = t[i++];
			switch( e ){
			case '"': *out += '"'; break;
			case '\\': *out += '\\'; break;
			case '/': *out += '/'; break;
			case 'b': *out += '\b'; break;
			case 'f': *out += '\f'; break;
			case 'n': *out += '\n'; break;
			case 'r': *out += '\r'; break;
			case 't': *out += '\t'; break;
			case 'u': {
				uint32_t cp;
				if( !Hex4( &cp ) ) return false;
				if( cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i+1] == 'u' ){
					i += 2;
					uint32_t lo;
					if( !Hex4( &lo ) ) return false;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				PutUtf8( *out, cp );
				break;
			}
			default: return false;
			}
		}
		return false;
	}

	bool Val( Value* out )
	{
		if( ++depth > 200 ) return false;
		Ws();
		if( i >= t.size() ) return false;
		char c = t[i];
		bool ok = true;
		if( c == '{' ){
			++i;
			*out = Value::MakeObject();
			Ws();
			if( i < t.size() && t[i] == '}' ){ ++i; }
			else for( ;; ){
				Ws();
				std::string key;
				if( i >= t.size() || !Str( &key ) ){ ok = false; break; }
				Ws();
				if( i >= t.size() || t[i] != ':' ){ ok = false; break; }
				++i;
				Value v;
				if( !Val( &v ) ){ ok = false; break; }
				out->Set( key, std::move( v ) );
				Ws();
				if( i < t.size() && t[i] == ',' ){ ++i; continue; }
				if( i < t.size() && t[i] == '}' ){ ++i; break; }
				ok = false;
				break;
			}
		} else if( c == '[' ){
			++i;
			*out = Value::MakeArray();
			Ws();
			if( i < t.size() && t[i] == ']' ){ ++i; }
			else for( ;; ){
				Value v;
				if( !Val( &v ) ){ ok = false; break; }
				out->Push( std::move( v ) );
				Ws();
				if( i < t.size() && t[i] == ',' ){ ++i; continue; }
				if( i < t.size() && t[i] == ']' ){ ++i; break; }
				ok = false;
				break;
			}
		} else if( c == '"' ){
			std::string s;
			ok = Str( &s );
			*out = Value( s );
		} else if( t.compare( i, 4, "true" ) == 0 ){ i += 4; *out = Value( true ); }
		else if( t.compare( i, 5, "false" ) == 0 ){ i += 5; *out = Value( false ); }
		else if( t.compare( i, 4, "null" ) == 0 ){ i += 4; *out = Value(); }
		else {
			const char* b = t.c_str() + i;
			char* e = nullptr;
			double d = std::strtod( b, &e );
			if( e == b ) ok = false;
			else { i += (size_t)(e - b); *out = Value( d ); }
		}
		--depth;
		return ok;
	}
};

}	// namespace

bool Parse( const std::string& text, Value* out )
{
	Reader r( text );
	if( !r.Val( out ) ) return false;
	r.Ws();
	return r.i == text.size();
}

}	// namespace json
}	// namespace ats
