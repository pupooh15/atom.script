/**************************************************************************/
/*!	\file	ats_diag.cpp
	\brief	診断の出力
***************************************************************************/
#include "ats_diag.h"

#include <cstdio>

namespace ats {
namespace compiler {

std::string Diagnostics::ToText() const
{
	std::string out;
	for( const Diagnostic& d : m_list ){
		out += d.file;
		if( d.line > 0 ) out += "(" + std::to_string( d.line ) + "," + std::to_string( d.col ) + ")";
		out += d.severity == Diagnostic::Error ? ": error: " : ": warning: ";
		out += d.message;
		out += "\n";
	}
	return out;
}

static std::string JsonStr( const std::string& s )
{
	std::string o = "\"";
	for( unsigned char c : s ){
		switch( c ){
		case '"':	o += "\\\""; break;
		case '\\':	o += "\\\\"; break;
		case '\n':	o += "\\n"; break;
		case '\r':	o += "\\r"; break;
		case '\t':	o += "\\t"; break;
		default:
			if( c < 0x20 ){ char buf[8]; snprintf( buf, sizeof(buf), "\\u%04x", c ); o += buf; }
			else o += (char)c;
		}
	}
	return o + "\"";
}

std::string Diagnostics::ToJson() const
{
	std::string out = "[";
	for( size_t i = 0; i < m_list.size(); ++i ){
		const Diagnostic& d = m_list[i];
		if( i ) out += ",";
		out += "{\"file\":" + JsonStr( d.file ) +
			   ",\"line\":" + std::to_string( d.line ) +
			   ",\"col\":" + std::to_string( d.col ) +
			   ",\"severity\":\"" + (d.severity == Diagnostic::Error ? "error" : "warning") + "\"" +
			   ",\"message\":" + JsonStr( d.message ) + "}";
	}
	return out + "]";
}

}	// namespace compiler
}	// namespace ats
