/**************************************************************************/
/*!	\file	ats_yaml.cpp
	\brief	マニフェスト用の YAML サブセット解析器
***************************************************************************/
#include "ats_yaml.h"

namespace ats {
namespace yaml {

const Node* Node::Get( const std::string& key ) const
{
	if( kind != Map ) return nullptr;
	for( const auto& kv : map ) if( kv.first == key ) return &kv.second;
	return nullptr;
}

std::string Node::Str( const std::string& key, const std::string& def ) const
{
	const Node* n = Get( key );
	return (n && n->kind == Scalar) ? n->scalar : def;
}

namespace {

struct Line {
	int			indent;
	std::string	text;		// インデントとコメントを除いた中身
	int			number;		// 1 始まり
};

// コメントを取り除く（クォートの中の # は残す）
std::string StripComment( const std::string& s )
{
	char quote = 0;
	for( size_t i = 0; i < s.size(); ++i ){
		char c = s[i];
		if( quote ){
			if( c == '\\' && quote == '"' ){ ++i; continue; }
			if( c == quote ) quote = 0;
		} else if( c == '"' || c == '\'' ){
			quote = c;
		} else if( c == '#' && (i == 0 || s[i-1] == ' ' || s[i-1] == '\t') ){
			return s.substr( 0, i );
		}
	}
	return s;
}

std::string TrimRight( const std::string& s )
{
	size_t e = s.find_last_not_of( " \t\r" );
	return e == std::string::npos ? "" : s.substr( 0, e + 1 );
}

std::string Trim( const std::string& s )
{
	size_t b = s.find_first_not_of( " \t\r" );
	if( b == std::string::npos ) return "";
	return TrimRight( s.substr( b ) );
}

class Parser {
public:
	Parser( std::vector<Line>&& lines, std::string* err ) : m_lines( std::move( lines ) ), m_err( err ) {}

	bool ParseDocument( Node* out )
	{
		if( m_lines.empty() ){ out->kind = Node::Null; return true; }
		if( !ParseBlock( m_lines[0].indent, out ) ) return false;
		if( m_pos < m_lines.size() ) return Fail( m_lines[m_pos].number, "インデントが不正です" );
		return true;
	}

private:
	bool Fail( int line, const std::string& msg )
	{
		if( m_err && m_err->empty() ) *m_err = std::to_string( line ) + ": " + msg;
		return false;
	}

	static bool IsSeqItem( const std::string& t ) { return t == "-" || (t.size() >= 2 && t[0] == '-' && t[1] == ' '); }

	// "key: value" のコロン位置（なければ npos）
	static size_t FindKeyColon( const std::string& t )
	{
		char quote = 0;
		int depth = 0;
		for( size_t i = 0; i < t.size(); ++i ){
			char c = t[i];
			if( quote ){
				if( c == '\\' && quote == '"' ){ ++i; continue; }
				if( c == quote ) quote = 0;
				continue;
			}
			if( c == '"' || c == '\'' ){ if( i == 0 ) quote = c; continue; }
			if( c == '{' || c == '[' ){ if( i == 0 ) return std::string::npos; ++depth; continue; }
			if( c == '}' || c == ']' ){ --depth; continue; }
			if( c == ':' && depth == 0 && (i + 1 == t.size() || t[i+1] == ' ') ) return i;
		}
		return std::string::npos;
	}

	bool ParseBlock( int indent, Node* out )
	{
		if( m_pos >= m_lines.size() ){ out->kind = Node::Null; return true; }
		out->line = m_lines[m_pos].number;
		if( IsSeqItem( m_lines[m_pos].text ) ) return ParseSeq( indent, out );
		return ParseMap( indent, out );
	}

	bool ParseMap( int indent, Node* out )
	{
		out->kind = Node::Map;
		while( m_pos < m_lines.size() && m_lines[m_pos].indent == indent && !IsSeqItem( m_lines[m_pos].text ) ){
			Line& ln = m_lines[m_pos];
			size_t colon = FindKeyColon( ln.text );
			if( colon == std::string::npos ) return Fail( ln.number, "「キー: 値」の形式ではありません" );
			std::string key = Trim( ln.text.substr( 0, colon ) );
			if( key.size() >= 2 && (key[0] == '"' || key[0] == '\'') && key.back() == key[0] ) key = key.substr( 1, key.size() - 2 );
			std::string rest = Trim( ln.text.substr( colon + 1 ) );
			for( const auto& kv : out->map )
				if( kv.first == key ) return Fail( ln.number, "キー '" + key + "' が重複しています" );

			Node value;
			value.line = ln.number;
			++m_pos;
			if( rest.empty() ){
				if( m_pos < m_lines.size() && m_lines[m_pos].indent > indent ){
					if( !ParseBlock( m_lines[m_pos].indent, &value ) ) return false;
				} else if( m_pos < m_lines.size() && m_lines[m_pos].indent == indent && IsSeqItem( m_lines[m_pos].text ) ){
					// key:\n- a（キーと同じ深さのシーケンス）
					if( !ParseSeq( indent, &value ) ) return false;
				}
			} else {
				if( !ParseInline( rest, ln.number, &value ) ) return false;
			}
			out->map.emplace_back( key, std::move( value ) );
		}
		if( m_pos < m_lines.size() && m_lines[m_pos].indent > indent )
			return Fail( m_lines[m_pos].number, "インデントが不正です" );
		return true;
	}

	bool ParseSeq( int indent, Node* out )
	{
		out->kind = Node::Seq;
		while( m_pos < m_lines.size() && m_lines[m_pos].indent == indent && IsSeqItem( m_lines[m_pos].text ) ){
			Line& ln = m_lines[m_pos];
			std::string rest = ln.text.size() > 1 ? ln.text.substr( 2 ) : "";
			size_t lead = rest.find_first_not_of( ' ' );
			int itemIndent = indent + 2 + (lead == std::string::npos ? 0 : (int)lead);
			rest = Trim( rest );

			Node item;
			item.line = ln.number;
			if( rest.empty() ){
				++m_pos;
				if( m_pos < m_lines.size() && m_lines[m_pos].indent > indent ){
					if( !ParseBlock( m_lines[m_pos].indent, &item ) ) return false;
				}
			} else if( FindKeyColon( rest ) != std::string::npos ){
				// "- key: value" はその位置から始まるマップとして読む
				ln.indent = itemIndent;
				ln.text   = rest;
				if( !ParseMap( itemIndent, &item ) ) return false;
			} else {
				++m_pos;
				if( !ParseInline( rest, ln.number, &item ) ) return false;
			}
			out->seq.push_back( std::move( item ) );
		}
		return true;
	}

	// 1 行の値（フロー形式は括弧が閉じるまで次の行を連結する）
	bool ParseInline( std::string text, int line, Node* out )
	{
		if( text[0] == '[' || text[0] == '{' ){
			while( !Balanced( text ) && m_pos < m_lines.size() ){
				text += " " + m_lines[m_pos].text;
				++m_pos;
			}
			size_t i = 0;
			if( !ParseFlow( text, i, line, out ) ) return false;
			SkipSpace( text, i );
			if( i != text.size() ) return Fail( line, "フロー形式の後ろに余分な文字があります" );
			return true;
		}
		if( text[0] == '|' || text[0] == '>' ) return Fail( line, "ブロックスカラー（| >）には対応していません" );
		if( text[0] == '&' || text[0] == '*' || text[0] == '!' ) return Fail( line, "アンカー・エイリアス・タグには対応していません" );
		return ParseScalar( text, line, out );
	}

	static bool Balanced( const std::string& t )
	{
		int depth = 0;
		char quote = 0;
		for( size_t i = 0; i < t.size(); ++i ){
			char c = t[i];
			if( quote ){
				if( c == '\\' && quote == '"' ){ ++i; continue; }
				if( c == quote ) quote = 0;
				continue;
			}
			if( c == '"' || c == '\'' ) quote = c;
			else if( c == '[' || c == '{' ) ++depth;
			else if( c == ']' || c == '}' ) --depth;
		}
		return depth <= 0;
	}

	static void SkipSpace( const std::string& t, size_t& i ) { while( i < t.size() && (t[i] == ' ' || t[i] == '\t') ) ++i; }

	bool ParseScalar( const std::string& text, int line, Node* out )
	{
		out->kind = Node::Scalar;
		out->line = line;
		if( text[0] == '"' || text[0] == '\'' ){
			size_t i = 0;
			if( !ReadQuoted( text, i, line, &out->scalar ) ) return false;
			if( Trim( text.substr( i ) ).size() ) return Fail( line, "クォートの後ろに余分な文字があります" );
			out->quoted = true;
			return true;
		}
		out->scalar = text;
		if( text == "~" || text == "null" ){ out->kind = Node::Null; out->scalar.clear(); }
		return true;
	}

	bool ReadQuoted( const std::string& t, size_t& i, int line, std::string* out )
	{
		char q = t[i++];
		out->clear();
		while( i < t.size() ){
			char c = t[i++];
			if( q == '\'' ){
				if( c == '\'' ){
					if( i < t.size() && t[i] == '\'' ){ out->push_back( '\'' ); ++i; continue; }
					return true;
				}
				out->push_back( c );
			} else {
				if( c == '"' ) return true;
				if( c == '\\' && i < t.size() ){
					char e = t[i++];
					switch( e ){
					case 'n': out->push_back( '\n' ); break;
					case 't': out->push_back( '\t' ); break;
					default:  out->push_back( e ); break;
					}
					continue;
				}
				out->push_back( c );
			}
		}
		return Fail( line, "クォートが閉じていません" );
	}

	bool ParseFlow( const std::string& t, size_t& i, int line, Node* out )
	{
		SkipSpace( t, i );
		if( i >= t.size() ) return Fail( line, "値がありません" );
		out->line = line;
		char c = t[i];
		if( c == '[' ){
			out->kind = Node::Seq;
			++i;
			SkipSpace( t, i );
			if( i < t.size() && t[i] == ']' ){ ++i; return true; }
			for( ;; ){
				Node item;
				if( !ParseFlow( t, i, line, &item ) ) return false;
				out->seq.push_back( std::move( item ) );
				SkipSpace( t, i );
				if( i < t.size() && t[i] == ',' ){ ++i; continue; }
				if( i < t.size() && t[i] == ']' ){ ++i; return true; }
				return Fail( line, "']' がありません" );
			}
		}
		if( c == '{' ){
			out->kind = Node::Map;
			++i;
			SkipSpace( t, i );
			if( i < t.size() && t[i] == '}' ){ ++i; return true; }
			for( ;; ){
				SkipSpace( t, i );
				std::string key;
				if( i < t.size() && (t[i] == '"' || t[i] == '\'') ){
					if( !ReadQuoted( t, i, line, &key ) ) return false;
				} else {
					size_t b = i;
					while( i < t.size() && t[i] != ':' && t[i] != ',' && t[i] != '}' ) ++i;
					key = Trim( t.substr( b, i - b ) );
				}
				SkipSpace( t, i );
				if( i >= t.size() || t[i] != ':' ) return Fail( line, "フロー形式のマップに ':' がありません" );
				++i;
				Node value;
				SkipSpace( t, i );
				if( i < t.size() && (t[i] == ',' || t[i] == '}') ) value.kind = Node::Null;
				else if( !ParseFlow( t, i, line, &value ) ) return false;
				for( const auto& kv : out->map )
					if( kv.first == key ) return Fail( line, "キー '" + key + "' が重複しています" );
				out->map.emplace_back( key, std::move( value ) );
				SkipSpace( t, i );
				if( i < t.size() && t[i] == ',' ){ ++i; continue; }
				if( i < t.size() && t[i] == '}' ){ ++i; return true; }
				return Fail( line, "'}' がありません" );
			}
		}
		if( c == '"' || c == '\'' ){
			out->kind   = Node::Scalar;
			out->quoted = true;
			return ReadQuoted( t, i, line, &out->scalar );
		}
		size_t b = i;
		while( i < t.size() && t[i] != ',' && t[i] != ']' && t[i] != '}' ) ++i;
		out->kind   = Node::Scalar;
		out->scalar = Trim( t.substr( b, i - b ) );
		if( out->scalar == "~" || out->scalar == "null" ){ out->kind = Node::Null; out->scalar.clear(); }
		return true;
	}

	std::vector<Line>	m_lines;
	size_t				m_pos = 0;
	std::string*		m_err;
};

}	// namespace

bool Parse( const std::string& text, Node* out, std::string* error )
{
	std::vector<Line> lines;
	size_t start = 0;
	int number = 0;
	// UTF-8 BOM を読み飛ばす
	if( text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF ) start = 3;
	while( start <= text.size() ){
		size_t end = text.find( '\n', start );
		if( end == std::string::npos ) end = text.size();
		++number;
		std::string raw = text.substr( start, end - start );
		start = end + 1;
		if( raw.find( '\t' ) != std::string::npos && raw.find_first_not_of( " \t" ) != std::string::npos &&
			raw.find_first_not_of( " " ) < raw.size() && raw[ raw.find_first_not_of( " " ) ] == '\t' ){
			if( error ) *error = std::to_string( number ) + ": インデントにタブは使えません";
			return false;
		}
		std::string s = TrimRight( StripComment( raw ) );
		size_t b = s.find_first_not_of( ' ' );
		if( b == std::string::npos ) continue;
		if( s.compare( b, 3, "---" ) == 0 && Trim( s ) == "---" ) continue;
		lines.push_back( { (int)b, s.substr( b ), number } );
		if( end == text.size() ) break;
	}
	if( error ) error->clear();
	Parser p( std::move( lines ), error );
	*out = Node();
	return p.ParseDocument( out );
}

}	// namespace yaml
}	// namespace ats
