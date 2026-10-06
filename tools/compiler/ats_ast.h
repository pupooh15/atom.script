/**************************************************************************/
/*!	\file	ats_ast.h
	\brief	.ats の構文木
***************************************************************************/
#ifndef ATS_AST_H
#define ATS_AST_H

#include "ats_lexer.h"

#include <memory>
#include <string>
#include <vector>

namespace ats {
namespace compiler {

struct Pos { int line = 0; int col = 0; };

//=========================================================================
// 式
//=========================================================================
struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Arg {
	std::string	name;		// 名前指定なら引数名（位置指定なら空）
	ExprPtr		value;
	Pos			pos;
};

struct Expr {
	enum Kind { IntLit, FloatLit, StrLit, BoolLit, Name, Member, Call, Unary, Binary };
	Kind				kind;
	Pos					pos;
	int64_t				ival = 0;
	double				fval = 0.0;
	std::string			text;		// Name / Member の右側 / Call の名前 / StrLit
	std::string			owner;		// Member の左側（バンク名・enum 名）
	Tok					op = Tok::End;
	ExprPtr				a, b;
	std::vector<Arg>	args;
	bool				await = false;
};

//=========================================================================
// 文
//=========================================================================
struct Stmt;
using StmtPtr = std::unique_ptr<Stmt>;

struct Block {
	std::vector<StmtPtr>	stmts;
	Pos						open;
	Pos						close;
};

struct Case {
	ExprPtr		value;		// nullptr なら default
	Block		body;
	Pos			pos;
};

struct Stmt {
	enum Kind {
		Let, Assign, ExprStmt, If, Switch, Loop, Break, Continue, Return,
		Parallel, Wait, WaitFrames, Yield, Fire, ResetVars, BlockStmt
	};
	Kind				kind;
	Pos					pos;
	std::string			node;		// @node の ID
	std::string			name;		// Let の変数名 / Fire のイベント名
	std::string			typeName;	// Let の型（省略可）
	Pos					typePos;
	ExprPtr				target;		// Assign の左辺
	Tok					op = Tok::Assign;
	ExprPtr				value;		// Let / Assign / ExprStmt / If の条件 / Switch の値 / Loop の回数 / Return / Wait
	Block				body;		// If の then / Loop / BlockStmt
	std::unique_ptr<Stmt> elseStmt;	// If の else（BlockStmt か If）
	std::vector<Case>	cases;		// Switch
	std::vector<Block>	branches;	// Parallel
	bool				race = false;
	std::vector<Arg>	args;		// Fire
};

//=========================================================================
// 宣言
//=========================================================================
struct Param {
	std::string	name;
	std::string	typeName;
	Pos			pos;
};

struct VarDecl {
	std::string	name;
	std::string	typeName;
	ExprPtr		init;
	bool		transient = false;
	std::string	was;
	Pos			pos;
	Pos			typePos;
};

struct FuncDecl {
	bool				isEvent = false;
	std::string			name;
	std::vector<Param>	params;
	std::string			retType;	// 空なら void
	Pos					retPos;
	Block				body;
	Pos					pos;
};

struct Script {
	std::string				scriptId;
	Pos						scriptPos;
	std::vector<VarDecl>	vars;
	std::vector<FuncDecl>	funcs;		// イベントと関数（出現順）
};

}	// namespace compiler
}	// namespace ats

#endif	// ATS_AST_H
