//=========================================================================
//	基本型（ats_api.h の enum と小さな値型）
//=========================================================================
using System;
using AtomScript.Interop;

namespace AtomScript
{
	// ats_result
	public enum AtsResult
	{
		Ok = 0,
		InvalidArg,			// 引数が不正
		OutOfMemory,		// メモリ確保失敗
		BadFormat,			// バイトコード／セーブデータの形式が不正
		Version,			// 形式バージョン不一致
		Unresolved,			// 未登録のコマンド・変数がある
		Signature,			// コマンドの引数シグネチャ不一致
		NotFound,			// 名前・ID が見つからない
		StaleId,			// 終了済みのファイバ・トークン
		Type,				// 型が合わない
		Busy,				// update 中など、今は実行できない
		Duplicate,			// 登録済み
	}

	// ats_type
	public enum AtsType
	{
		Void = 0,
		Bool,
		Int,
		Float,
		String,
		Enum,
		Handle,
	}

	// ats_status（コマンドハンドラの戻り値）
	public enum AtsStatus
	{
		Done = 0,			// その場で完了
		Pending,			// 待機。後で ScriptVM.Complete / Fail を呼ぶ
		Running,			// 待機。次の update で同じハンドラをもう一度呼ぶ
		Fail,				// 失敗
	}

	// ats_var_scope
	public enum AtsVarScope
	{
		Persistent = 0,		// セーブ対象
		Session,			// セーブしない
	}

	public enum AtsLogLevel
	{
		Info = 0,
		Warning,
		Error,
	}

	// handle 型の値（意味はホストが決める 64bit 値。0 は無効）
	[Serializable]
	public readonly struct AtsHandle : IEquatable<AtsHandle>
	{
		public readonly ulong Value;

		public AtsHandle( ulong value ) { Value = value; }

		public bool IsValid => Value != 0;
		public static readonly AtsHandle None = default;

		public bool Equals( AtsHandle o ) => Value == o.Value;
		public override bool Equals( object o ) => o is AtsHandle h && Equals( h );
		public override int GetHashCode() => Value.GetHashCode();
		public override string ToString() => $"AtsHandle({Value})";
		public static bool operator ==( AtsHandle a, AtsHandle b ) => a.Value == b.Value;
		public static bool operator !=( AtsHandle a, AtsHandle b ) => a.Value != b.Value;
	}

	// ファイバの ID（世代番号付き。終了後に使っても安全に失敗する）
	public readonly struct FiberId : IEquatable<FiberId>
	{
		public readonly ulong Value;

		public FiberId( ulong value ) { Value = value; }

		public bool IsValid => Value != 0;
		public static readonly FiberId None = default;

		public bool Equals( FiberId o ) => Value == o.Value;
		public override bool Equals( object o ) => o is FiberId f && Equals( f );
		public override int GetHashCode() => Value.GetHashCode();
		public override string ToString() => $"FiberId({Value:x})";
		public static bool operator ==( FiberId a, FiberId b ) => a.Value == b.Value;
		public static bool operator !=( FiberId a, FiberId b ) => a.Value != b.Value;
	}

	// 型付きの共有変数 ID（atsc gen が Vars に生成する）
	public readonly struct VarId<T>
	{
		public readonly uint Id;

		public VarId( uint id ) { Id = id; }

		public override string ToString() => $"VarId<{typeof( T ).Name}>({Id})";
	}

	// AtomScript の API が失敗したときの例外
	public class AtsException : Exception
	{
		public AtsResult Result { get; }

		public AtsException( AtsResult result, string message )
			: base( string.IsNullOrEmpty( message ) ? ResultString( result ) : $"{message} ({ResultString( result )})" )
		{
			Result = result;
		}

		internal static string ResultString( AtsResult r ) => Utf8.ToManaged( NativeMethods.ats_result_string( (int)r ) );
	}
}
