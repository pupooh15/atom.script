/**************************************************************************/
/*!	\file	ats_format.h
	\brief	AtomScript バイナリ形式（.atsb）の定義
	\note
	VM とコンパイラ（atsc）が共有する。詳細は docs/bytecode.md。
	すべてリトルエンディアン。構造体はパディングなしで並べる。
***************************************************************************/
#ifndef ATS_FORMAT_H
#define ATS_FORMAT_H

#include <stdint.h>

namespace ats {
namespace fmt {

constexpr uint32_t FourCC( char a, char b, char c, char d )
{
	return (uint32_t)(uint8_t)a | ((uint32_t)(uint8_t)b << 8) |
		   ((uint32_t)(uint8_t)c << 16) | ((uint32_t)(uint8_t)d << 24);
}

//=========================================================================
// ヘッダー
//=========================================================================
constexpr uint32_t	kMagic			= FourCC( 'A', 'T', 'S', 'B' );
constexpr uint16_t	kVersionMajor	= 1;
constexpr uint16_t	kVersionMinor	= 0;
constexpr uint32_t	kNone			= 0xFFFFFFFFu;	// 「なし」を表す文字列インデックス

struct FileHeader {
	uint32_t	magic;
	uint16_t	version_major;
	uint16_t	version_minor;
	uint32_t	flags;
	uint32_t	file_size;
	uint64_t	manifest_hash;
	uint32_t	script_id;		// STRS のインデックス
	uint32_t	section_count;
};
static_assert( sizeof(FileHeader) == 32, "FileHeader size" );

struct SectionEntry {
	uint32_t	tag;
	uint32_t	offset;			// ファイル先頭からのオフセット
	uint32_t	size;
	uint32_t	reserved;
};
static_assert( sizeof(SectionEntry) == 16, "SectionEntry size" );

constexpr uint32_t kSecStrings	= FourCC( 'S', 'T', 'R', 'S' );
constexpr uint32_t kSecConsts	= FourCC( 'C', 'N', 'S', 'T' );
constexpr uint32_t kSecImports	= FourCC( 'I', 'M', 'P', 'T' );
constexpr uint32_t kSecVars		= FourCC( 'V', 'A', 'R', 'S' );
constexpr uint32_t kSecEntries	= FourCC( 'E', 'N', 'T', 'R' );
constexpr uint32_t kSecCode		= FourCC( 'C', 'O', 'D', 'E' );
constexpr uint32_t kSecDebug	= FourCC( 'D', 'B', 'U', 'G' );

//=========================================================================
// セクションの要素
//	STRS : u32 count, u32 offset[count]（セクション先頭から）, 以降 NUL 終端文字列
//	CNST : u32 count, u64 value[count]
//	IMPT : u32 count, ImportEntry[count]
//	VARS : u32 count, VarEntry[count]
//	ENTR : u32 count, EntryEntry[count]
//	CODE : 命令列
//	DBUG : u32 count, DebugEntry[count]（pc の昇順）
//=========================================================================
constexpr int kMaxParams = 8;

enum ImportKind : uint8_t {
	kImportCommand	= 0,
	kImportQuery	= 1,
};

struct ImportEntry {
	uint32_t	name;					// STRS
	uint8_t		kind;					// ImportKind
	uint8_t		argc;
	uint8_t		retc;					// 0 か 1
	uint8_t		flags;					// 予約
	uint32_t	sig_hash;				// 0 なら検査しない
	uint8_t		param_types[kMaxParams];// ats_type
	uint8_t		ret_type;
	uint8_t		pad[3];
};
static_assert( sizeof(ImportEntry) == 24, "ImportEntry size" );

enum VarKind : uint8_t {
	kVarShared	= 0,	// マニフェストの共有変数（id で参照）
	kVarScript	= 1,	// スクリプト変数（name で参照）
};

enum VarFlags : uint8_t {
	kVarTransient	= 1 << 0,	// セーブしない
};

struct VarEntry {
	uint8_t		kind;		// VarKind
	uint8_t		type;		// ats_type
	uint8_t		flags;		// VarFlags
	uint8_t		pad;
	uint32_t	id;			// 共有変数の安定ID
	uint32_t	name;		// STRS（スクリプト変数の名前）
	uint32_t	was_name;	// STRS（@was の旧名）。なければ kNone
	uint64_t	init;		// 初期値（STRING の場合は STRS のインデックス）
};
static_assert( sizeof(VarEntry) == 24, "VarEntry size" );

enum EntryKind : uint8_t {
	kEntryEvent		= 0,
	kEntryFunction	= 1,
};

struct EntryEntry {
	uint32_t	name;					// STRS
	uint32_t	code;					// CODE 内のオフセット
	uint8_t		kind;					// EntryKind
	uint8_t		paramc;
	uint8_t		retc;					// 0 か 1
	uint8_t		ret_type;
	uint16_t	localc;					// 引数を含むローカル数
	uint16_t	max_stack;				// 演算スタックの最大深さ
	uint8_t		param_types[kMaxParams];
};
static_assert( sizeof(EntryEntry) == 24, "EntryEntry size" );

struct DebugEntry {
	uint32_t	pc;
	uint32_t	line;
	uint32_t	node;					// STRS（@node の ID）。なければ kNone
};
static_assert( sizeof(DebugEntry) == 12, "DebugEntry size" );

//=========================================================================
// 命令
//	1 バイトのオペコード + 固定長オペランド。スタック要素は 64bit。
//	ジャンプのオフセットは「次の命令の先頭」からの相対値。
//=========================================================================
enum Op : uint8_t {
	OP_NOP = 0,

	// 定数・スタック
	OP_PUSH_I32,		// i32
	OP_PUSH_F32,		// f32
	OP_PUSH_K,			// u32 定数インデックス
	OP_PUSH_STR,		// u32 文字列インデックス
	OP_POP,
	OP_DUP,

	// ローカル・変数
	OP_LD_LOCAL,		// u16
	OP_ST_LOCAL,		// u16
	OP_LD_VAR,			// u16 VARS インデックス
	OP_ST_VAR,			// u16

	// 整数演算
	OP_ADD_I, OP_SUB_I, OP_MUL_I, OP_DIV_I, OP_MOD_I, OP_NEG_I,
	// 実数演算
	OP_ADD_F, OP_SUB_F, OP_MUL_F, OP_DIV_F, OP_NEG_F,
	// ビット演算・論理否定
	OP_BAND, OP_BOR, OP_BXOR, OP_SHL, OP_SHR, OP_BNOT, OP_NOT,
	// 比較
	OP_EQ_I, OP_NE_I, OP_LT_I, OP_LE_I, OP_GT_I, OP_GE_I,
	OP_EQ_F, OP_NE_F, OP_LT_F, OP_LE_F, OP_GT_F, OP_GE_F,
	OP_EQ_H, OP_NE_H,
	// 変換
	OP_I2F, OP_F2I,

	// 分岐
	OP_JMP,				// i32
	OP_JZ,				// i32
	OP_JNZ,				// i32
	OP_SWITCH,			// u16 count, i32 default, { i32 value, i32 offset } * count

	// 呼び出し
	OP_CALL_CMD,		// u16 インポート, u8 argc
	OP_CALL_QUERY,		// u16 インポート, u8 argc
	OP_CALL_FN,			// u16 エントリ
	OP_RET,

	// 並行・待機
	OP_FORK,			// i32 子ファイバの開始位置
	OP_JOIN,			// 子ファイバがすべて終わるまで待つ
	OP_RACE,			// 子ファイバのどれかが終わるまで待ち、残りを中断する
	OP_YIELD,			// 次のフレームまで待つ
	OP_SLEEP,			// 秒数（float）を取り出して待つ
	OP_WAIT_FRAMES,		// フレーム数（int）を取り出して待つ
	OP_END,				// ファイバ終了

	// その他
	OP_FIRE,			// u32 イベント名（STRS）, u8 argc
	OP_RESET_VARS,		// このスクリプトの var を初期値に戻す
	OP_RAND,			// 上限（int）を取り出し、[0, 上限) の乱数を積む

	OP_COUNT
};

// オペランドのバイト数（SWITCH は可変なので固定部分のみ）
inline int OperandSize( uint8_t op )
{
	switch( op ){
	case OP_PUSH_I32: case OP_PUSH_F32: case OP_PUSH_K: case OP_PUSH_STR:
	case OP_JMP: case OP_JZ: case OP_JNZ: case OP_FORK:
		return 4;
	case OP_LD_LOCAL: case OP_ST_LOCAL: case OP_LD_VAR: case OP_ST_VAR:
	case OP_CALL_FN:
		return 2;
	case OP_CALL_CMD: case OP_CALL_QUERY:
		return 3;
	case OP_FIRE:
		return 5;
	case OP_SWITCH:
		return 6;
	default:
		return 0;
	}
}

}	// namespace fmt
}	// namespace ats

#endif	// ATS_FORMAT_H
