/**************************************************************************/
/*!	\file	ats_parser.h
	\brief	.ats の構文解析
***************************************************************************/
#ifndef ATS_PARSER_H
#define ATS_PARSER_H

#include "ats_ast.h"

namespace ats {
namespace compiler {

// 構文解析。エラーは diag に入れ、文単位で回復して続ける
bool Parse( const std::string& src, const std::string& file, Script* out, Diagnostics& diag );

}	// namespace compiler
}	// namespace ats

#endif	// ATS_PARSER_H
