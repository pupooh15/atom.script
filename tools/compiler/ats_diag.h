/**************************************************************************/
/*!	\file	ats_diag.h
	\brief	コンパイラの診断（エラー・警告）
***************************************************************************/
#ifndef ATS_DIAG_H
#define ATS_DIAG_H

#include <string>
#include <vector>

namespace ats {
namespace compiler {

struct Diagnostic {
	enum Severity { Error, Warning };
	Severity	severity;
	std::string	file;
	int			line;
	int			col;
	std::string	message;
};

class Diagnostics {
public:
	void Error( const std::string& file, int line, int col, const std::string& msg )
	{
		m_list.push_back( { Diagnostic::Error, file, line, col, msg } );
	}
	void Warning( const std::string& file, int line, int col, const std::string& msg )
	{
		m_list.push_back( { Diagnostic::Warning, file, line, col, msg } );
	}
	bool HasErrors() const
	{
		for( const Diagnostic& d : m_list ) if( d.severity == Diagnostic::Error ) return true;
		return false;
	}
	size_t ErrorCount() const
	{
		size_t n = 0;
		for( const Diagnostic& d : m_list ) if( d.severity == Diagnostic::Error ) ++n;
		return n;
	}
	const std::vector<Diagnostic>& List() const { return m_list; }
	void Append( const Diagnostics& o ) { m_list.insert( m_list.end(), o.m_list.begin(), o.m_list.end() ); }

	// "file(line,col): error: message"（VS / VS Code で飛べる形式）
	std::string ToText() const;
	// [{"file":..,"line":..,"col":..,"severity":"error","message":..}]
	std::string ToJson() const;

private:
	std::vector<Diagnostic> m_list;
};

}	// namespace compiler
}	// namespace ats

#endif	// ATS_DIAG_H
