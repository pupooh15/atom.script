/**************************************************************************/
/*!	\file	ats_api.h
	\brief	AtomScript コア C API
	\note
	エンジン・ゲームはこのヘッダーの関数だけを使ってコアを操作する。
	例外・STL・RTTI はこの境界を越えない。文字列は UTF-8。
***************************************************************************/
#ifndef ATS_API_H
#define ATS_API_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*=========================================================================*/
/* エクスポート */
/*=========================================================================*/
#if defined(ATS_STATIC)
	#define ATS_API
#elif defined(_WIN32)
	#if defined(ATS_BUILD_DLL)
		#define ATS_API __declspec(dllexport)
	#else
		#define ATS_API __declspec(dllimport)
	#endif
#else
	#define ATS_API __attribute__((visibility("default")))
#endif

#if defined(_WIN32)
	#define ATS_CALL __cdecl
#else
	#define ATS_CALL
#endif

/* API バージョン（major が違えば非互換） */
#define ATS_API_VERSION_MAJOR	1
#define ATS_API_VERSION_MINOR	0

/*=========================================================================*/
/* 基本型 */
/*=========================================================================*/
typedef struct ats_runtime	ats_runtime;
typedef struct ats_program	ats_program;
typedef struct ats_vm		ats_vm;
typedef struct ats_call		ats_call;

/* 世代番号付き ID。終了後に使われても安全に失敗する */
typedef uint64_t ats_fiber_id;
typedef uint64_t ats_call_token;

#define ATS_INVALID_ID	((uint64_t)0)

/* 結果コード */
typedef enum ats_result {
	ATS_OK = 0,
	ATS_ERR_INVALID_ARG,		/* 引数が不正 */
	ATS_ERR_OUT_OF_MEMORY,		/* メモリ確保失敗 */
	ATS_ERR_BAD_FORMAT,			/* バイトコード／セーブデータの形式が不正 */
	ATS_ERR_VERSION,			/* 形式バージョン不一致 */
	ATS_ERR_UNRESOLVED,			/* 未登録のコマンド・変数がある */
	ATS_ERR_SIGNATURE,			/* コマンドの引数シグネチャ不一致 */
	ATS_ERR_NOT_FOUND,			/* 名前・ID が見つからない */
	ATS_ERR_STALE_ID,			/* 終了済みのファイバ・トークン */
	ATS_ERR_TYPE,				/* 型が合わない */
	ATS_ERR_BUSY,				/* update 中など、今は実行できない */
	ATS_ERR_DUPLICATE			/* 登録済み */
} ats_result;

/* 値の型 */
typedef enum ats_type {
	ATS_TYPE_VOID = 0,
	ATS_TYPE_BOOL,
	ATS_TYPE_INT,
	ATS_TYPE_FLOAT,
	ATS_TYPE_STRING,
	ATS_TYPE_ENUM,		/* 値は int。名前はマニフェスト側 */
	ATS_TYPE_HANDLE		/* ホストが意味を決める 64bit 値 */
} ats_type;

/* 値 */
typedef struct ats_value {
	uint32_t	type;		/* ats_type */
	uint32_t	reserved;
	union {
		int32_t		i;		/* INT / ENUM */
		int32_t		b;		/* BOOL（0/1） */
		float		f;		/* FLOAT */
		const char*	s;		/* STRING（VM 内部の文字列。次の update まで有効） */
		uint64_t	h;		/* HANDLE */
	} v;
} ats_value;

/* コマンドハンドラの戻り値 */
typedef enum ats_status {
	ATS_DONE = 0,		/* その場で完了。次の命令へ */
	ATS_PENDING,		/* 待機。後で ats_call_complete / ats_call_fail を呼ぶ */
	ATS_RUNNING,		/* 待機。次の update で同じハンドラをもう一度呼ぶ */
	ATS_FAIL			/* 失敗 */
} ats_status;

/* ログレベル */
typedef enum ats_log_level {
	ATS_LOG_INFO = 0,
	ATS_LOG_WARNING,
	ATS_LOG_ERROR
} ats_log_level;

/*=========================================================================*/
/* コールバック */
/*=========================================================================*/
typedef void*		(ATS_CALL *ats_alloc_fn)(size_t size, size_t align, void* user);
typedef void		(ATS_CALL *ats_free_fn)(void* ptr, void* user);
typedef void		(ATS_CALL *ats_log_fn)(ats_log_level level, const char* message, void* user);

typedef ats_status	(ATS_CALL *ats_command_fn)(ats_call* call, void* user);
/* トークンは VM ごとに振られるので、どの VM の呼び出しかも渡す */
typedef void		(ATS_CALL *ats_cancel_fn)(ats_vm* vm, ats_call_token token, void* user);
typedef ats_result	(ATS_CALL *ats_query_fn)(ats_call* call, void* user);

typedef ats_result	(ATS_CALL *ats_write_fn)(const void* data, size_t size, void* user);
typedef ats_result	(ATS_CALL *ats_read_fn)(void* data, size_t size, void* user);

/*=========================================================================*/
/* 記述子（先頭の size に sizeof を入れる。将来のフィールド追加に備える） */
/*=========================================================================*/
typedef struct ats_runtime_desc {
	uint32_t		size;
	ats_alloc_fn	alloc;			/* NULL なら標準の malloc */
	ats_free_fn		free;
	ats_log_fn		log;			/* NULL ならログを捨てる */
	void*			user;
} ats_runtime_desc;

typedef struct ats_command_desc {
	uint32_t		size;
	const char*		name;			/* マニフェストの name */
	uint32_t		sig_hash;		/* 0 ならシグネチャを検査しない */
	const char*		channel;		/* NULL なら直列化しない */
	ats_command_fn	fn;
	ats_cancel_fn	cancel;			/* NULL 可 */
	void*			user;
} ats_command_desc;

typedef struct ats_query_desc {
	uint32_t		size;
	const char*		name;
	uint32_t		sig_hash;
	ats_query_fn	fn;
	void*			user;
} ats_query_desc;

/* 変数バンクのスコープ */
typedef enum ats_var_scope {
	ATS_SCOPE_PERSISTENT = 0,	/* セーブ対象 */
	ATS_SCOPE_SESSION			/* セーブしない */
} ats_var_scope;

/* 共有変数（マニフェストの variable_banks から生成して登録する） */
typedef struct ats_var_desc {
	uint32_t		size;
	uint32_t		id;				/* 安定ID */
	const char*		name;			/* "story.chapter" のようなバンク付きの名前 */
	uint32_t		type;			/* ats_type */
	uint32_t		scope;			/* ats_var_scope */
	ats_value		init;
} ats_var_desc;

typedef struct ats_vm_desc {
	uint32_t		size;
	uint32_t		instruction_budget;	/* 1 回の update で実行する命令数の上限。0 なら既定値 */
	uint32_t		max_stack;			/* ファイバ 1 本のスタック要素数。0 なら既定値 */
	uint32_t		max_call_depth;		/* 関数呼び出しの深さ。0 なら既定値 */
	uint64_t		random_seed;
} ats_vm_desc;

/*=========================================================================*/
/* バージョン・エラー */
/*=========================================================================*/
ATS_API uint32_t	ATS_CALL ats_get_api_version(void);		/* (major << 16) | minor */
ATS_API const char*	ATS_CALL ats_result_string(ats_result r);

/*=========================================================================*/
/* ランタイム（アプリ全体で 1 つ。初期化後は登録内容を変えない） */
/*=========================================================================*/
ATS_API ats_result	ATS_CALL ats_runtime_create(const ats_runtime_desc* desc, ats_runtime** out);
ATS_API void		ATS_CALL ats_runtime_destroy(ats_runtime* rt);
ATS_API ats_result	ATS_CALL ats_register_command(ats_runtime* rt, const ats_command_desc* desc);
ATS_API ats_result	ATS_CALL ats_register_query(ats_runtime* rt, const ats_query_desc* desc);
ATS_API ats_result	ATS_CALL ats_define_var(ats_runtime* rt, const ats_var_desc* desc);
ATS_API const char*	ATS_CALL ats_runtime_last_error(ats_runtime* rt);

/*=========================================================================*/
/* プログラム（.atsb の読み込み単位。読み取り専用で複数 VM から共有できる） */
/*=========================================================================*/
/* bytes は呼び出し中だけ参照する（内部にコピーする） */
ATS_API ats_result	ATS_CALL ats_program_load(ats_runtime* rt, const void* bytes, size_t size, ats_program** out);
ATS_API void		ATS_CALL ats_program_release(ats_program* prog);
ATS_API const char*	ATS_CALL ats_program_script_id(const ats_program* prog);

/*=========================================================================*/
/* VM */
/*=========================================================================*/
ATS_API ats_result	ATS_CALL ats_vm_create(ats_runtime* rt, const ats_vm_desc* desc, ats_vm** out);
ATS_API void		ATS_CALL ats_vm_destroy(ats_vm* vm);
ATS_API const char*	ATS_CALL ats_vm_last_error(ats_vm* vm);

/* プログラムを VM に結び付ける（スクリプト変数の確保）。fire_event でも自動で行う */
ATS_API ats_result	ATS_CALL ats_vm_attach(ats_vm* vm, ats_program* prog);
/* 実行中のファイバを中断してから外す。スクリプト変数の値は VM に残る */
ATS_API ats_result	ATS_CALL ats_vm_detach(ats_vm* vm, ats_program* prog);

ATS_API ats_result	ATS_CALL ats_vm_update(ats_vm* vm, float dt);
ATS_API ats_result	ATS_CALL ats_vm_fire_event(ats_vm* vm, ats_program* prog, const char* event,
										   const ats_value* args, int argc, ats_fiber_id* out);
/* 結び付いている全プログラムの同名イベントを発火する。発火したファイバ数を out_count に返す */
ATS_API ats_result	ATS_CALL ats_vm_broadcast_event(ats_vm* vm, const char* event,
												const ats_value* args, int argc, int* out_count);
ATS_API ats_result	ATS_CALL ats_vm_abort_fiber(ats_vm* vm, ats_fiber_id fiber);
ATS_API int			ATS_CALL ats_vm_is_fiber_alive(ats_vm* vm, ats_fiber_id fiber);
ATS_API int			ATS_CALL ats_vm_fiber_count(ats_vm* vm);

/*=========================================================================*/
/* コマンド・クエリのハンドラから使う関数 */
/*=========================================================================*/
ATS_API int			ATS_CALL ats_call_argc(ats_call* call);
ATS_API ats_value	ATS_CALL ats_call_arg(ats_call* call, int index);
ATS_API int32_t		ATS_CALL ats_arg_int(ats_call* call, int index);
ATS_API float		ATS_CALL ats_arg_float(ats_call* call, int index);
ATS_API int			ATS_CALL ats_arg_bool(ats_call* call, int index);
ATS_API const char*	ATS_CALL ats_arg_string(ats_call* call, int index);
ATS_API uint64_t	ATS_CALL ats_arg_handle(ats_call* call, int index);
ATS_API ats_result	ATS_CALL ats_call_set_result(ats_call* call, const ats_value* value);
/* ATS_PENDING を返す前に呼ぶ。返したトークンで後から完了させる */
ATS_API ats_call_token	ATS_CALL ats_call_get_token(ats_call* call);
ATS_API ats_fiber_id	ATS_CALL ats_call_fiber(ats_call* call);
ATS_API ats_vm*			ATS_CALL ats_call_vm(ats_call* call);
/* ATS_RUNNING で呼び直されたとき、何回目の呼び出しか（初回は 0） */
ATS_API uint32_t		ATS_CALL ats_call_retry_count(ats_call* call);

/*=========================================================================*/
/* 待機中の呼び出しの完了 */
/*=========================================================================*/
ATS_API ats_result	ATS_CALL ats_call_complete(ats_vm* vm, ats_call_token token, const ats_value* result);
ATS_API ats_result	ATS_CALL ats_call_fail(ats_vm* vm, ats_call_token token, const char* reason);

/*=========================================================================*/
/* 共有変数 */
/*=========================================================================*/
ATS_API ats_result	ATS_CALL ats_var_get(ats_vm* vm, uint32_t var_id, ats_value* out);
ATS_API ats_result	ATS_CALL ats_var_set(ats_vm* vm, uint32_t var_id, const ats_value* value);

/*=========================================================================*/
/* スクリプト変数 */
/*=========================================================================*/
/* 指定スクリプトの var をすべて初期値に戻す（ニューゲーム、クエスト破棄など） */
ATS_API ats_result	ATS_CALL ats_script_reset(ats_vm* vm, const char* script_id);

/*=========================================================================*/
/* セーブ（保存されるのは変数と乱数の状態だけ。実行中のファイバは保存しない） */
/*=========================================================================*/
ATS_API ats_result	ATS_CALL ats_vm_save(ats_vm* vm, ats_write_fn write, void* user);
ATS_API ats_result	ATS_CALL ats_vm_load(ats_vm* vm, ats_read_fn read, void* user);

/*=========================================================================*/
/* デバッグ用 API（ATS_ENABLE_DEBUG ビルドのみ） */
/*=========================================================================*/
#if defined(ATS_ENABLE_DEBUG)
ATS_API ats_result	ATS_CALL ats_debug_script_var_get(ats_vm* vm, const char* script_id, const char* name, ats_value* out);
ATS_API ats_result	ATS_CALL ats_debug_script_var_set(ats_vm* vm, const char* script_id, const char* name, const ats_value* value);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ATS_API_H */
