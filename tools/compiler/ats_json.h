/**************************************************************************/
/*!	\file	ats_json.h
	\brief	最小限の JSON（言語サーバーの JSON-RPC 用）
***************************************************************************/
#ifndef ATS_JSON_H
#define ATS_JSON_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ats {
namespace json {

class Value {
public:
	enum Kind { Null, Bool, Number, String, Array, Object };

	Value() = default;
	Value( std::nullptr_t ) {}
	Value( bool b ) : m_kind( Bool ), m_bool( b ) {}
	Value( int v ) : m_kind( Number ), m_num( v ) {}
	Value( int64_t v ) : m_kind( Number ), m_num( (double)v ) {}
	Value( double v ) : m_kind( Number ), m_num( v ) {}
	Value( const char* s ) : m_kind( String ), m_str( s ) {}
	Value( const std::string& s ) : m_kind( String ), m_str( s ) {}

	static Value MakeArray()  { Value v; v.m_kind = Array; return v; }
	static Value MakeObject() { Value v; v.m_kind = Object; return v; }

	Kind			kind() const	{ return m_kind; }
	bool			IsNull() const	{ return m_kind == Null; }
	bool			IsObject() const{ return m_kind == Object; }
	bool			IsArray() const	{ return m_kind == Array; }
	bool			IsString() const{ return m_kind == String; }
	bool			IsNumber() const{ return m_kind == Number; }

	bool				AsBool( bool def = false ) const			{ return m_kind == Bool ? m_bool : def; }
	double				AsNumber( double def = 0 ) const			{ return m_kind == Number ? m_num : def; }
	int					AsInt( int def = 0 ) const					{ return m_kind == Number ? (int)m_num : def; }
	const std::string&	AsString() const							{ return m_str; }

	// オブジェクト
	const Value&	operator[]( const char* key ) const;
	Value&			Set( const std::string& key, Value v );
	bool			Has( const char* key ) const;
	const std::vector<std::pair<std::string, Value>>& Members() const { return m_obj; }

	// 配列
	const Value&	operator[]( size_t i ) const { return i < m_arr.size() ? m_arr[i] : Empty(); }
	const Value&	operator[]( int i ) const { return i >= 0 ? (*this)[(size_t)i] : Empty(); }	// v[0] を曖昧にしない
	size_t			Size() const { return m_kind == Array ? m_arr.size() : m_obj.size(); }
	Value&			Push( Value v ) { m_kind = Array; m_arr.push_back( std::move( v ) ); return m_arr.back(); }

	std::string		Dump() const;

private:
	static const Value& Empty();
	void DumpTo( std::string& out ) const;

	Kind										m_kind = Null;
	bool										m_bool = false;
	double										m_num = 0;
	std::string									m_str;
	std::vector<Value>							m_arr;
	std::vector<std::pair<std::string, Value>>	m_obj;
};

// 解析。失敗したら false
bool Parse( const std::string& text, Value* out );

}	// namespace json
}	// namespace ats

#endif	// ATS_JSON_H
