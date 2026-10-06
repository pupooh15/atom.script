/**************************************************************************/
/*!	\file	ats_lexer.cpp
	\brief	.ats の字句解析
***************************************************************************/
#include "ats_lexer.h"

#include <cctype>
#include <cstdlib>

namespace ats {
namespace compiler {

const char* TokName( Tok t )
{
	switch( t ){
	case Tok::End:		return "ファイルの終わり";
	case Tok::Ident:	return "識別子";
	case Tok::Int:		return "整数";
	case Tok::Float:	return "実数";
	case Tok::String:	return "文字列";
	case Tok::LParen:	return "'('";
	case Tok::RParen:	return "')'";
	case Tok::LBrace:	return "'{'";
	case Tok::RBrace:	return "'}'";
	case Tok::LBracket:	return "'['";
	case Tok::RBracket:	return "']'";
	case Tok::Comma:	return "','";
	case Tok::Colon:	return "':'";
	case Tok::Dot:		return "'.'";
	case Tok::Arrow:	return "'->'";
	case Tok::At:		return "'@'";
	case Tok::Assign:	return "'='";
	default:			return "記号";
	}
}

std::vector<Token> Tokenize( const std::string& src, const std::string& file, Diagnostics& diag )
{
	std::vector<Token> out;
	size_t i = 0;
	int line = 1;
	size_t lineStart = 0;
	const size_t n = src.size();

	// UTF-8 BOM
	if( n >= 3 && (unsigned char)src[0] == 0xEF && (unsigned char)src[1] == 0xBB && (unsigned char)src[2] == 0xBF ){ i = 3; lineStart = 3; }

	auto col = [&]( size_t pos ) {
		// 列は文字数で数える（UTF-8 の継続バイトは数えない）
		int c = 1;
		for( size_t k = lineStart; k < pos; ++k ) if( ((unsigned char)src[k] & 0xC0) != 0x80 ) ++c;
		return c;
	};

	while( i < n ){
		char c = src[i];
		// 空白・改行
		if( c == '\n' ){ ++line; ++i; lineStart = i; continue; }
		if( c == ' ' || c == '\t' || c == '\r' ){ ++i; continue; }
		// コメント
		if( c == '/' && i + 1 < n && src[i+1] == '/' ){
			while( i < n && src[i] != '\n' ) ++i;
			continue;
		}
		if( c == '/' && i + 1 < n && src[i+1] == '*' ){
			int startLine = line, startCol = col( i );
			i += 2;
			bool closed = false;
			while( i < n ){
				if( src[i] == '*' && i + 1 < n && src[i+1] == '/' ){ i += 2; closed = true; break; }
				if( src[i] == '\n' ){ ++line; lineStart = i + 1; }
				++i;
			}
			if( !closed ) diag.Error( file, startLine, startCol, "コメント /* が閉じていません" );
			continue;
		}

		Token t;
		t.line = line;
		t.col  = col( i );

		// 識別子（ASCII 英字・数字・_、および UTF-8 の非 ASCII 文字）
		if( (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || ((unsigned char)c & 0x80) ){
			size_t b = i;
			while( i < n ){
				char d = src[i];
				if( (d >= 'a' && d <= 'z') || (d >= 'A' && d <= 'Z') || (d >= '0' && d <= '9') || d == '_' || ((unsigned char)d & 0x80) ) ++i;
				else break;
			}
			t.kind = Tok::Ident;
			t.text = src.substr( b, i - b );
			out.push_back( t );
			continue;
		}

		// 数値
		if( c >= '0' && c <= '9' ){
			size_t b = i;
			bool isFloat = false;
			if( c == '0' && i + 1 < n && (src[i+1] == 'x' || src[i+1] == 'X') ){
				i += 2;
				while( i < n && isxdigit( (unsigned char)src[i] ) ) ++i;
			} else {
				while( i < n && src[i] >= '0' && src[i] <= '9' ) ++i;
				if( i + 1 < n && src[i] == '.' && src[i+1] >= '0' && src[i+1] <= '9' ){
					isFloat = true;
					++i;
					while( i < n && src[i] >= '0' && src[i] <= '9' ) ++i;
				}
				if( i < n && (src[i] == 'e' || src[i] == 'E') ){
					size_t k = i + 1;
					if( k < n && (src[k] == '+' || src[k] == '-') ) ++k;
					if( k < n && src[k] >= '0' && src[k] <= '9' ){
						isFloat = true;
						i = k;
						while( i < n && src[i] >= '0' && src[i] <= '9' ) ++i;
					}
				}
			}
			std::string num = src.substr( b, i - b );
			if( isFloat ){
				t.kind = Tok::Float;
				t.fval = std::strtod( num.c_str(), nullptr );
			} else {
				t.kind = Tok::Int;
				t.ival = (int64_t)std::strtoull( num.c_str(), nullptr, 0 );
				if( t.ival > 0xFFFFFFFFll ) diag.Error( file, t.line, t.col, "整数 " + num + " が大きすぎます" );
			}
			t.text = num;
			out.push_back( t );
			continue;
		}

		// 文字列
		if( c == '"' ){
			++i;
			std::string s;
			bool closed = false;
			while( i < n ){
				char d = src[i++];
				if( d == '"' ){ closed = true; break; }
				if( d == '\n' ){ break; }
				if( d == '\\' && i < n ){
					char e = src[i++];
					switch( e ){
					case 'n':	s.push_back( '\n' ); break;
					case 't':	s.push_back( '\t' ); break;
					case '"':	s.push_back( '"' ); break;
					case '\\':	s.push_back( '\\' ); break;
					default:
						diag.Error( file, line, col( i - 2 ), std::string( "未知のエスケープ \\" ) + e );
						break;
					}
					continue;
				}
				s.push_back( d );
			}
			if( !closed ){
				diag.Error( file, t.line, t.col, "文字列が閉じていません" );
				if( i > 0 && src[i-1] == '\n' ){ ++line; lineStart = i; }
			}
			t.kind = Tok::String;
			t.text = s;
			out.push_back( t );
			continue;
		}

		// 記号
		auto two = [&]( char a, char b ) { return c == a && i + 1 < n && src[i+1] == b; };
		auto three = [&]( const char* s ) { return i + 2 < n && src[i] == s[0] && src[i+1] == s[1] && src[i+2] == s[2]; };
		Tok k = Tok::End;
		int len = 1;
		if( three( "<<=" ) )		{ k = Tok::ShlAssign; len = 3; }
		else if( three( ">>=" ) )	{ k = Tok::ShrAssign; len = 3; }
		else if( two( '-', '>' ) )	{ k = Tok::Arrow; len = 2; }
		else if( two( '&', '&' ) )	{ k = Tok::AndAnd; len = 2; }
		else if( two( '|', '|' ) )	{ k = Tok::OrOr; len = 2; }
		else if( two( '=', '=' ) )	{ k = Tok::Eq; len = 2; }
		else if( two( '!', '=' ) )	{ k = Tok::Ne; len = 2; }
		else if( two( '<', '=' ) )	{ k = Tok::Le; len = 2; }
		else if( two( '>', '=' ) )	{ k = Tok::Ge; len = 2; }
		else if( two( '<', '<' ) )	{ k = Tok::Shl; len = 2; }
		else if( two( '>', '>' ) )	{ k = Tok::Shr; len = 2; }
		else if( two( '+', '=' ) )	{ k = Tok::PlusAssign; len = 2; }
		else if( two( '-', '=' ) )	{ k = Tok::MinusAssign; len = 2; }
		else if( two( '*', '=' ) )	{ k = Tok::StarAssign; len = 2; }
		else if( two( '/', '=' ) )	{ k = Tok::SlashAssign; len = 2; }
		else if( two( '%', '=' ) )	{ k = Tok::PercentAssign; len = 2; }
		else if( two( '&', '=' ) )	{ k = Tok::AmpAssign; len = 2; }
		else if( two( '|', '=' ) )	{ k = Tok::PipeAssign; len = 2; }
		else if( two( '^', '=' ) )	{ k = Tok::CaretAssign; len = 2; }
		else switch( c ){
			case '(': k = Tok::LParen; break;
			case ')': k = Tok::RParen; break;
			case '{': k = Tok::LBrace; break;
			case '}': k = Tok::RBrace; break;
			case '[': k = Tok::LBracket; break;
			case ']': k = Tok::RBracket; break;
			case ',': k = Tok::Comma; break;
			case ':': k = Tok::Colon; break;
			case '.': k = Tok::Dot; break;
			case '@': k = Tok::At; break;
			case '+': k = Tok::Plus; break;
			case '-': k = Tok::Minus; break;
			case '*': k = Tok::Star; break;
			case '/': k = Tok::Slash; break;
			case '%': k = Tok::Percent; break;
			case '&': k = Tok::Amp; break;
			case '|': k = Tok::Pipe; break;
			case '^': k = Tok::Caret; break;
			case '~': k = Tok::Tilde; break;
			case '!': k = Tok::Bang; break;
			case '<': k = Tok::Lt; break;
			case '>': k = Tok::Gt; break;
			case '=': k = Tok::Assign; break;
			default: break;
		}
		if( k == Tok::End ){
			diag.Error( file, t.line, t.col, std::string( "使えない文字 '" ) + c + "'" );
			++i;
			continue;
		}
		t.kind = k;
		t.text = src.substr( i, (size_t)len );
		i += (size_t)len;
		out.push_back( t );
	}

	Token end;
	end.kind = Tok::End;
	end.line = line;
	end.col  = col( i );
	out.push_back( end );
	return out;
}

}	// namespace compiler
}	// namespace ats
