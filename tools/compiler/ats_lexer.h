/**************************************************************************/
/*!	\file	ats_lexer.h
	\brief	.ats の字句解析
***************************************************************************/
#ifndef ATS_LEXER_H
#define ATS_LEXER_H

#include "ats_diag.h"

#include <cstdint>
#include <string>
#include <vector>

namespace ats {
namespace compiler {

enum class Tok {
	End,
	Ident,
	Int,
	Float,
	String,
	// 記号
	LParen, RParen, LBrace, RBrace, LBracket, RBracket,
	Comma, Colon, Dot, Arrow, At,
	Plus, Minus, Star, Slash, Percent,
	Amp, Pipe, Caret, Tilde, Bang, Shl, Shr,
	AndAnd, OrOr,
	Eq, Ne, Lt, Le, Gt, Ge,
	Assign, PlusAssign, MinusAssign, StarAssign, SlashAssign, PercentAssign,
	AmpAssign, PipeAssign, CaretAssign, ShlAssign, ShrAssign,
};

struct Token {
	Tok			kind = Tok::End;
	std::string	text;		// 識別子・文字列（エスケープ解決済み）
	int64_t		ival = 0;
	double		fval = 0.0;
	int			line = 0;
	int			col = 0;
};

// 字句解析。エラーは diag に入れ、続行できる範囲で続ける
std::vector<Token> Tokenize( const std::string& src, const std::string& file, Diagnostics& diag );

const char* TokName( Tok t );

}	// namespace compiler
}	// namespace ats

#endif	// ATS_LEXER_H
