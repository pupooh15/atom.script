/**************************************************************************/
/*!	\file	ats_parser.cpp
	\brief	.ats の構文解析
	\note
	文の区切りは改行。式は括弧の中を除いて行をまたがない
	（「a」の次の行の「(b)」を呼び出しと誤解しないため）。
***************************************************************************/
#include "ats_parser.h"

namespace ats {
namespace compiler {

namespace {

const char* const kKeywords[] = {
	"script", "var", "transient", "let", "event", "fn", "if", "else", "switch", "case", "default",
	"loop", "break", "continue", "return", "parallel", "race", "wait", "yield", "await", "fire",
	"reset", "true", "false",
};

bool IsKeyword( const std::string& s )
{
	for( const char* k : kKeywords ) if( s == k ) return true;
	return false;
}

class Parser {
public:
	Parser( std::vector<Token>&& toks, const std::string& file, Diagnostics& diag )
		: m_t( std::move( toks ) ), m_file( file ), m_diag( diag ) {}

	bool ParseScript( Script* out );

private:
	//---------------------------------------------------------------------
	// トークン操作
	//---------------------------------------------------------------------
	const Token& Peek( int k = 0 ) const
	{
		size_t i = m_i + (size_t)k;
		return i < m_t.size() ? m_t[i] : m_t.back();
	}
	const Token& Prev() const { return m_t[ m_i ? m_i - 1 : 0 ]; }
	const Token& Next() { const Token& t = m_t[m_i]; if( m_i + 1 < m_t.size() ) ++m_i; return t; }
	bool Is( Tok k ) const { return Peek().kind == k; }
	bool IsWord( const char* w ) const { return Peek().kind == Tok::Ident && Peek().text == w; }
	bool SameLine() const { return Peek().line == Prev().line; }
	Pos  PosOf( const Token& t ) const { Pos p; p.line = t.line; p.col = t.col; return p; }

	void Error( const Token& t, const std::string& msg )
	{
		if( m_panic ) return;
		m_diag.Error( m_file, t.line, t.col, msg );
		m_panic = true;
	}

	bool Expect( Tok k, const char* what )
	{
		if( Is( k ) ){ Next(); return true; }
		Error( Peek(), std::string( what ) + " が必要です（" + Describe( Peek() ) + " があります）" );
		return false;
	}

	bool ExpectWord( const char* w )
	{
		if( IsWord( w ) ){ Next(); return true; }
		Error( Peek(), std::string( "'" ) + w + "' が必要です" );
		return false;
	}

	bool ExpectIdent( std::string* out, const char* what )
	{
		if( Is( Tok::Ident ) && !IsKeyword( Peek().text ) ){ *out = Next().text; return true; }
		Error( Peek(), std::string( what ) + " が必要です（" + Describe( Peek() ) + " があります）" );
		return false;
	}

	static std::string Describe( const Token& t )
	{
		switch( t.kind ){
		case Tok::Ident:	return "'" + t.text + "'";
		case Tok::Int:
		case Tok::Float:	return t.text;
		case Tok::String:	return "文字列";
		case Tok::End:		return "ファイルの終わり";
		default:			return "'" + t.text + "'";
		}
	}

	// 文の途中でエラーになったら、次の行の先頭か '}' まで読み飛ばす
	void Sync( int errorLine )
	{
		while( !Is( Tok::End ) && !Is( Tok::RBrace ) && Peek().line <= errorLine ) Next();
		m_panic = false;
	}

	//---------------------------------------------------------------------
	// 宣言
	//---------------------------------------------------------------------
	bool ParseParams( std::vector<Param>* out );
	bool ParseFunc( Script* s, bool isEvent );
	bool ParseVar( Script* s, const std::string& was, bool transient, const Token& start );

	//---------------------------------------------------------------------
	// 文
	//---------------------------------------------------------------------
	bool ParseBlock( Block* out );
	StmtPtr ParseStmt();
	StmtPtr ParseIf( const Token& start );
	bool ParseArgs( std::vector<Arg>* out );

	//---------------------------------------------------------------------
	// 式
	//---------------------------------------------------------------------
	ExprPtr ParseExpr() { return ParseBinary( 0 ); }
	ExprPtr ParseBinary( int level );
	ExprPtr ParseUnary();
	ExprPtr ParsePrimary();

	std::vector<Token>	m_t;
	size_t				m_i = 0;
	std::string			m_file;
	Diagnostics&		m_diag;
	bool				m_panic = false;
	int					m_paren = 0;	// 括弧の深さ（中では行をまたげる）
};

//=========================================================================
// 宣言
//=========================================================================
bool Parser::ParseScript( Script* out )
{
	// script "id"
	if( !IsWord( "script" ) ){
		Error( Peek(), "ファイルの先頭に script \"<スクリプトID>\" を書いてください" );
		m_panic = false;
	} else {
		const Token& kw = Next();
		out->scriptPos = PosOf( kw );
		if( Is( Tok::String ) ) out->scriptId = Next().text;
		else { Error( Peek(), "スクリプトID（文字列）が必要です" ); Sync( kw.line ); }
	}

	while( !Is( Tok::End ) ){
		const Token& start = Peek();
		std::string was;
		bool transient = false;

		// 注釈
		while( Is( Tok::At ) ){
			Next();
			std::string name;
			if( !ExpectIdent( &name, "注釈名" ) ) break;
			if( !Expect( Tok::LParen, "'('" ) ) break;
			if( !Is( Tok::String ) ){ Error( Peek(), "注釈の引数は文字列です" ); break; }
			std::string arg = Next().text;
			if( !Expect( Tok::RParen, "')'" ) ) break;
			if( name == "was" ) was = arg;
			else Error( start, "ここで使える注釈は @was だけです" );
		}
		if( m_panic ){ Sync( start.line ); continue; }

		if( IsWord( "transient" ) ){ Next(); transient = true; }

		bool ok;
		if( IsWord( "var" ) )			ok = ParseVar( out, was, transient, start );
		else if( transient || !was.empty() ){
			Error( Peek(), "@was / transient は var の前にだけ書けます" );
			ok = false;
		}
		else if( IsWord( "event" ) )	ok = ParseFunc( out, true );
		else if( IsWord( "fn" ) )		ok = ParseFunc( out, false );
		else if( IsWord( "script" ) ){	Error( Peek(), "script はファイルの先頭に 1 つだけ書けます" ); ok = false; }
		else {
			Error( Peek(), "var / event / fn のどれかが必要です（" + Describe( Peek() ) + " があります）" );
			ok = false;
		}
		if( !ok ){
			// 次の宣言の先頭まで読み飛ばす
			int depth = 0;
			while( !Is( Tok::End ) ){
				if( Is( Tok::LBrace ) ) ++depth;
				else if( Is( Tok::RBrace ) ){ if( --depth <= 0 ){ Next(); break; } }
				else if( depth == 0 && Peek().line > start.line &&
						 (IsWord( "var" ) || IsWord( "event" ) || IsWord( "fn" ) || Is( Tok::At ) || IsWord( "transient" )) ) break;
				Next();
			}
			m_panic = false;
		}
	}
	return !m_diag.HasErrors();
}

bool Parser::ParseVar( Script* s, const std::string& was, bool transient, const Token& start )
{
	Next();		// var
	VarDecl v;
	v.pos       = PosOf( start );
	v.was       = was;
	v.transient = transient;
	if( !ExpectIdent( &v.name, "変数名" ) ) return false;
	if( !Expect( Tok::Colon, "':'（型の指定）" ) ) return false;
	v.typePos = PosOf( Peek() );
	if( !ExpectIdent( &v.typeName, "型名" ) ) return false;
	if( !Expect( Tok::Assign, "'='（初期値）" ) ) return false;
	v.init = ParseExpr();
	if( !v.init ) return false;
	s->vars.push_back( std::move( v ) );
	return true;
}

bool Parser::ParseParams( std::vector<Param>* out )
{
	if( !Expect( Tok::LParen, "'('" ) ) return false;
	++m_paren;
	if( !Is( Tok::RParen ) ){
		for( ;; ){
			Param p;
			p.pos = PosOf( Peek() );
			if( !ExpectIdent( &p.name, "引数名" ) ){ --m_paren; return false; }
			if( !Expect( Tok::Colon, "':'（型の指定）" ) ){ --m_paren; return false; }
			if( !ExpectIdent( &p.typeName, "型名" ) ){ --m_paren; return false; }
			out->push_back( p );
			if( Is( Tok::Comma ) ){ Next(); continue; }
			break;
		}
	}
	--m_paren;
	return Expect( Tok::RParen, "')'" );
}

bool Parser::ParseFunc( Script* s, bool isEvent )
{
	const Token& kw = Next();
	FuncDecl f;
	f.isEvent = isEvent;
	f.pos     = PosOf( kw );
	if( !ExpectIdent( &f.name, isEvent ? "イベント名" : "関数名" ) ) return false;
	if( !ParseParams( &f.params ) ) return false;
	if( Is( Tok::Arrow ) ){
		Next();
		if( isEvent ){ Error( Prev(), "イベントは値を返せません" ); return false; }
		f.retPos = PosOf( Peek() );
		if( !ExpectIdent( &f.retType, "戻り値の型" ) ) return false;
	}
	if( !ParseBlock( &f.body ) ) return false;
	s->funcs.push_back( std::move( f ) );
	return true;
}

//=========================================================================
// 文
//=========================================================================
bool Parser::ParseBlock( Block* out )
{
	out->open = PosOf( Peek() );
	if( !Expect( Tok::LBrace, "'{'" ) ) return false;
	int savedParen = m_paren;
	m_paren = 0;
	while( !Is( Tok::RBrace ) && !Is( Tok::End ) ){
		int line = Peek().line;
		size_t before = m_i;
		StmtPtr s = ParseStmt();
		if( s ) out->stmts.push_back( std::move( s ) );
		else {
			Sync( line );
			if( m_i == before && !Is( Tok::RBrace ) && !Is( Tok::End ) ) Next();
		}
	}
	m_paren = savedParen;
	out->close = PosOf( Peek() );
	return Expect( Tok::RBrace, "'}'" );
}

bool Parser::ParseArgs( std::vector<Arg>* out )
{
	if( !Expect( Tok::LParen, "'('" ) ) return false;
	++m_paren;
	if( !Is( Tok::RParen ) ){
		for( ;; ){
			Arg a;
			a.pos = PosOf( Peek() );
			if( Is( Tok::Ident ) && Peek( 1 ).kind == Tok::Colon && !IsKeyword( Peek().text ) ){
				a.name = Next().text;
				Next();
			}
			a.value = ParseExpr();
			if( !a.value ){ --m_paren; return false; }
			out->push_back( std::move( a ) );
			if( Is( Tok::Comma ) ){ Next(); continue; }
			break;
		}
	}
	--m_paren;
	return Expect( Tok::RParen, "')'" );
}

StmtPtr Parser::ParseIf( const Token& start )
{
	StmtPtr s( new Stmt() );
	s->kind = Stmt::If;
	s->pos  = PosOf( start );
	if( !Expect( Tok::LParen, "'('" ) ) return nullptr;
	++m_paren;
	s->value = ParseExpr();
	--m_paren;
	if( !s->value || !Expect( Tok::RParen, "')'" ) ) return nullptr;
	if( !ParseBlock( &s->body ) ) return nullptr;
	if( IsWord( "else" ) ){
		Next();
		if( IsWord( "if" ) ){
			const Token& kw = Next();
			s->elseStmt = ParseIf( kw );
			if( !s->elseStmt ) return nullptr;
		} else {
			StmtPtr b( new Stmt() );
			b->kind = Stmt::BlockStmt;
			b->pos  = PosOf( Peek() );
			if( !ParseBlock( &b->body ) ) return nullptr;
			s->elseStmt = std::move( b );
		}
	}
	return s;
}

StmtPtr Parser::ParseStmt()
{
	std::string node;
	while( Is( Tok::At ) ){
		const Token& at = Next();
		std::string name;
		if( !ExpectIdent( &name, "注釈名" ) || !Expect( Tok::LParen, "'('" ) ) return nullptr;
		if( !Is( Tok::String ) ){ Error( Peek(), "注釈の引数は文字列です" ); return nullptr; }
		std::string arg = Next().text;
		if( !Expect( Tok::RParen, "')'" ) ) return nullptr;
		if( name != "node" ){ Error( at, "文に付けられる注釈は @node だけです" ); return nullptr; }
		node = arg;
	}

	const Token& start = Peek();
	StmtPtr s( new Stmt() );
	s->pos  = PosOf( start );
	s->node = node;

	if( IsWord( "let" ) ){
		Next();
		s->kind = Stmt::Let;
		if( !ExpectIdent( &s->name, "変数名" ) ) return nullptr;
		if( Is( Tok::Colon ) ){
			Next();
			s->typePos = PosOf( Peek() );
			if( !ExpectIdent( &s->typeName, "型名" ) ) return nullptr;
		}
		if( !Expect( Tok::Assign, "'='" ) ) return nullptr;
		s->value = ParseExpr();
		return s->value ? std::move( s ) : nullptr;
	}
	if( IsWord( "if" ) ){
		Next();
		StmtPtr r = ParseIf( start );
		if( r ) r->node = node;
		return r;
	}
	if( IsWord( "switch" ) ){
		Next();
		s->kind = Stmt::Switch;
		if( !Expect( Tok::LParen, "'('" ) ) return nullptr;
		++m_paren;
		s->value = ParseExpr();
		--m_paren;
		if( !s->value || !Expect( Tok::RParen, "')'" ) || !Expect( Tok::LBrace, "'{'" ) ) return nullptr;
		while( !Is( Tok::RBrace ) && !Is( Tok::End ) ){
			Case c;
			c.pos = PosOf( Peek() );
			if( IsWord( "case" ) ){
				Next();
				c.value = ParseExpr();
				if( !c.value ) return nullptr;
			} else if( IsWord( "default" ) ){
				Next();
			} else {
				Error( Peek(), "case か default が必要です" );
				return nullptr;
			}
			if( !ParseBlock( &c.body ) ) return nullptr;
			s->cases.push_back( std::move( c ) );
		}
		if( !Expect( Tok::RBrace, "'}'" ) ) return nullptr;
		return s;
	}
	if( IsWord( "loop" ) ){
		Next();
		s->kind = Stmt::Loop;
		if( Is( Tok::LParen ) ){
			Next();
			++m_paren;
			s->value = ParseExpr();
			--m_paren;
			if( !s->value || !Expect( Tok::RParen, "')'" ) ) return nullptr;
		}
		if( !ParseBlock( &s->body ) ) return nullptr;
		return s;
	}
	if( IsWord( "break" ) ){ Next(); s->kind = Stmt::Break; return s; }
	if( IsWord( "continue" ) ){ Next(); s->kind = Stmt::Continue; return s; }
	if( IsWord( "return" ) ){
		Next();
		s->kind = Stmt::Return;
		if( SameLine() && !Is( Tok::RBrace ) && !Is( Tok::End ) ){
			s->value = ParseExpr();
			if( !s->value ) return nullptr;
		}
		return s;
	}
	if( IsWord( "parallel" ) || IsWord( "race" ) ){
		s->kind = Stmt::Parallel;
		s->race = Next().text == "race";
		if( !Expect( Tok::LBrace, "'{'" ) ) return nullptr;
		while( !Is( Tok::RBrace ) && !Is( Tok::End ) ){
			if( !IsWord( "branch" ) ){ Error( Peek(), "branch が必要です" ); return nullptr; }
			Next();
			Block b;
			if( !ParseBlock( &b ) ) return nullptr;
			s->branches.push_back( std::move( b ) );
		}
		if( !Expect( Tok::RBrace, "'}'" ) ) return nullptr;
		return s;
	}
	if( IsWord( "wait" ) ){
		Next();
		if( IsWord( "frames" ) ){ Next(); s->kind = Stmt::WaitFrames; }
		else s->kind = Stmt::Wait;
		s->value = ParseExpr();
		return s->value ? std::move( s ) : nullptr;
	}
	if( IsWord( "yield" ) ){ Next(); s->kind = Stmt::Yield; return s; }
	if( IsWord( "fire" ) ){
		Next();
		s->kind = Stmt::Fire;
		if( !ExpectIdent( &s->name, "イベント名" ) ) return nullptr;
		if( !ParseArgs( &s->args ) ) return nullptr;
		return s;
	}
	if( IsWord( "reset" ) ){
		Next();
		if( !ExpectWord( "vars" ) ) return nullptr;
		s->kind = Stmt::ResetVars;
		return s;
	}
	if( Is( Tok::LBrace ) ){
		s->kind = Stmt::BlockStmt;
		if( !ParseBlock( &s->body ) ) return nullptr;
		return s;
	}
	if( Is( Tok::Ident ) && IsKeyword( Peek().text ) && Peek().text != "await" &&
		Peek().text != "true" && Peek().text != "false" ){
		Error( Peek(), "ここに '" + Peek().text + "' は書けません" );
		return nullptr;
	}

	// 代入か式文
	ExprPtr e = ParseExpr();
	if( !e ) return nullptr;
	switch( Peek().kind ){
	case Tok::Assign: case Tok::PlusAssign: case Tok::MinusAssign: case Tok::StarAssign:
	case Tok::SlashAssign: case Tok::PercentAssign: case Tok::AmpAssign: case Tok::PipeAssign:
	case Tok::CaretAssign: case Tok::ShlAssign: case Tok::ShrAssign:
		s->kind   = Stmt::Assign;
		s->op     = Next().kind;
		s->target = std::move( e );
		s->value  = ParseExpr();
		return s->value ? std::move( s ) : nullptr;
	default:
		break;
	}
	s->kind  = Stmt::ExprStmt;
	s->value = std::move( e );
	return s;
}

//=========================================================================
// 式
//=========================================================================
namespace {

struct BinLevel { Tok ops[6]; };

const BinLevel kLevels[] = {
	{ { Tok::OrOr } },
	{ { Tok::AndAnd } },
	{ { Tok::Pipe } },
	{ { Tok::Caret } },
	{ { Tok::Amp } },
	{ { Tok::Eq, Tok::Ne } },
	{ { Tok::Lt, Tok::Le, Tok::Gt, Tok::Ge } },
	{ { Tok::Shl, Tok::Shr } },
	{ { Tok::Plus, Tok::Minus } },
	{ { Tok::Star, Tok::Slash, Tok::Percent } },
};
const int kLevelCount = (int)(sizeof(kLevels) / sizeof(kLevels[0]));

}	// namespace

ExprPtr Parser::ParseBinary( int level )
{
	if( level >= kLevelCount ) return ParseUnary();
	ExprPtr left = ParseBinary( level + 1 );
	if( !left ) return nullptr;
	for( ;; ){
		Tok k = Peek().kind;
		bool match = false;
		for( Tok o : kLevels[level].ops ) if( o == k && o != Tok::End ) match = true;
		// 括弧の外では、行をまたいだ演算子は式の続きとみなさない
		if( !match || (m_paren == 0 && !SameLine()) ) return left;
		const Token& opTok = Next();
		ExprPtr right = ParseBinary( level + 1 );
		if( !right ) return nullptr;
		ExprPtr e( new Expr() );
		e->kind = Expr::Binary;
		e->pos  = PosOf( opTok );
		e->op   = k;
		e->a    = std::move( left );
		e->b    = std::move( right );
		left = std::move( e );
	}
}

ExprPtr Parser::ParseUnary()
{
	if( Is( Tok::Minus ) || Is( Tok::Bang ) || Is( Tok::Tilde ) ){
		const Token& opTok = Next();
		ExprPtr a = ParseUnary();
		if( !a ) return nullptr;
		ExprPtr e( new Expr() );
		e->kind = Expr::Unary;
		e->pos  = PosOf( opTok );
		e->op   = opTok.kind;
		e->a    = std::move( a );
		return e;
	}
	return ParsePrimary();
}

ExprPtr Parser::ParsePrimary()
{
	const Token& t = Peek();
	ExprPtr e( new Expr() );
	e->pos = PosOf( t );

	switch( t.kind ){
	case Tok::Int:		Next(); e->kind = Expr::IntLit; e->ival = t.ival; return e;
	case Tok::Float:	Next(); e->kind = Expr::FloatLit; e->fval = t.fval; return e;
	case Tok::String:	Next(); e->kind = Expr::StrLit; e->text = t.text; return e;
	case Tok::LParen: {
		Next();
		++m_paren;
		ExprPtr inner = ParseExpr();
		--m_paren;
		if( !inner || !Expect( Tok::RParen, "')'" ) ) return nullptr;
		return inner;
	}
	case Tok::Ident:
		break;
	default:
		Error( t, "式が必要です（" + Describe( t ) + " があります）" );
		return nullptr;
	}

	if( t.text == "true" || t.text == "false" ){
		Next();
		e->kind = Expr::BoolLit;
		e->ival = t.text == "true" ? 1 : 0;
		return e;
	}
	if( t.text == "await" ){
		Next();
		ExprPtr call = ParsePrimary();
		if( !call ) return nullptr;
		if( call->kind != Expr::Call ){ Error( t, "await の後ろには呼び出しを書いてください" ); return nullptr; }
		call->await = true;
		call->pos   = e->pos;
		return call;
	}
	if( IsKeyword( t.text ) ){
		Error( t, "式が必要です（'" + t.text + "' があります）" );
		return nullptr;
	}

	Next();
	if( Is( Tok::LParen ) && SameLine() ){
		e->kind = Expr::Call;
		e->text = t.text;
		if( !ParseArgs( &e->args ) ) return nullptr;
		return e;
	}
	if( Is( Tok::Dot ) ){
		Next();
		e->kind  = Expr::Member;
		e->owner = t.text;
		if( !ExpectIdent( &e->text, "メンバー名" ) ) return nullptr;
		return e;
	}
	e->kind = Expr::Name;
	e->text = t.text;
	return e;
}

}	// namespace

bool Parse( const std::string& src, const std::string& file, Script* out, Diagnostics& diag )
{
	size_t before = diag.ErrorCount();
	std::vector<Token> toks = Tokenize( src, file, diag );
	Parser p( std::move( toks ), file, diag );
	p.ParseScript( out );
	return diag.ErrorCount() == before;
}

}	// namespace compiler
}	// namespace ats
