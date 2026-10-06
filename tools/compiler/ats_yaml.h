/**************************************************************************/
/*!	\file	ats_yaml.h
	\brief	マニフェスト用の YAML サブセット解析器
	\note
	対応：ブロックのマップ・シーケンス、フロー形式 {…} […]、
	プレーン／クォート付きスカラー、# コメント。
	非対応：アンカー・エイリアス、ブロックスカラー（| >）、複数ドキュメント、タグ。
***************************************************************************/
#ifndef ATS_YAML_H
#define ATS_YAML_H

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ats {
namespace yaml {

struct Node {
	enum Kind { Null, Scalar, Map, Seq };
	Kind												kind = Null;
	std::string											scalar;
	bool												quoted = false;
	std::vector<std::pair<std::string, Node>>			map;
	std::vector<Node>									seq;
	int													line = 0;

	bool		IsNull() const	 { return kind == Null; }
	bool		IsScalar() const { return kind == Scalar; }
	bool		IsMap() const	 { return kind == Map; }
	bool		IsSeq() const	 { return kind == Seq; }
	const Node*	Get( const std::string& key ) const;
	std::string	Str( const std::string& key, const std::string& def = "" ) const;
};

// 失敗したら false を返し、error に「行: 内容」を入れる
bool Parse( const std::string& text, Node* out, std::string* error );

}	// namespace yaml
}	// namespace ats

#endif	// ATS_YAML_H
