//=========================================================================
//	ScriptVM：スクリプトの実行単位（ファイバ・変数・乱数を持つ）
//	メインスレッドで使う。Update は自分で呼ぶか、AtomScriptLoop.Register で毎フレーム呼ばせる。
//=========================================================================
using System;
using System.IO;
using System.Runtime.InteropServices;
using AOT;
using AtomScript.Interop;

namespace AtomScript
{
	public sealed class ScriptVM : IDisposable
	{
		readonly VmSafeHandle	m_handle = new VmSafeHandle();
		readonly IntPtr			m_ptr;

		public ScriptRuntime Runtime { get; }
		public bool IsDisposed => m_handle.IsClosed;

		internal IntPtr Ptr => m_handle.IsClosed ? throw new ObjectDisposedException( nameof( ScriptVM ) ) : m_ptr;

		internal ScriptVM( ScriptRuntime rt, IntPtr p )
		{
			Runtime = rt;
			m_ptr = p;
			m_handle.Set( p, rt.Handle );
		}

		// 実行中のファイバは中断され、待機中のコマンドにはキャンセルが届く
		public void Dispose()
		{
			if( m_handle.IsClosed ) return;
			AtomScriptLoop.Unregister( this );
			m_handle.Dispose();
			Runtime.OnVMDisposed( m_ptr );
		}

		public string LastError => Utf8.ToManaged( NativeMethods.ats_vm_last_error( Ptr ) );

		//---------------------------------------------------------------------
		// 実行
		//---------------------------------------------------------------------
		// 毎フレーム呼ぶ（例外は投げない）
		public AtsResult Update( float deltaTime ) => (AtsResult)NativeMethods.ats_vm_update( Ptr, deltaTime );

		public void Attach( ScriptProgram program ) => Check( NativeMethods.ats_vm_attach( Ptr, program.Ptr ), "プログラムを結び付けられません" );

		// 実行中のファイバを中断してから外す。スクリプト変数の値は VM に残る
		public void Detach( ScriptProgram program ) => Check( NativeMethods.ats_vm_detach( Ptr, program.Ptr ), "プログラムを外せません" );

		// イベントを発火する（未 Attach なら自動で結び付ける）
		public FiberId FireEvent( ScriptProgram program, string eventName, AtsValue[] args = null )
		{
			using( var ev = new Utf8( eventName ) )
			using( var a = new NativeArgs( args ) ){
				int r = NativeMethods.ats_vm_fire_event( Ptr, program.Ptr, ev.Ptr, a.Values, a.Count, out ulong fiber );
				Check( r, $"イベント '{eventName}' を発火できません" );
				return new FiberId( fiber );
			}
		}

		// 結び付いている全プログラムの同名イベントを発火する。発火したファイバ数を返す
		public int BroadcastEvent( string eventName, AtsValue[] args = null )
		{
			using( var ev = new Utf8( eventName ) )
			using( var a = new NativeArgs( args ) ){
				int r = NativeMethods.ats_vm_broadcast_event( Ptr, ev.Ptr, a.Values, a.Count, out int count );
				Check( r, $"イベント '{eventName}' を発火できません" );
				return count;
			}
		}

		// 終了済みのファイバなら false（例外は投げない）
		public bool AbortFiber( FiberId fiber ) => NativeMethods.ats_vm_abort_fiber( Ptr, fiber.Value ) == 0;
		public bool IsFiberAlive( FiberId fiber ) => NativeMethods.ats_vm_is_fiber_alive( Ptr, fiber.Value ) != 0;
		public int FiberCount => NativeMethods.ats_vm_fiber_count( Ptr );

		//---------------------------------------------------------------------
		// 待機中の呼び出しの完了（AtsCall.GetToken を使う低レベル API。例外は投げない）
		//---------------------------------------------------------------------
		public AtsResult Complete( ulong token ) => (AtsResult)NativeMethods.ats_call_complete_void( Ptr, token, IntPtr.Zero );

		public AtsResult Complete( ulong token, AtsValue result )
		{
			NativeValue n = result.ToNative( out IntPtr str );
			try {
				return (AtsResult)NativeMethods.ats_call_complete( Ptr, token, ref n );
			} finally {
				if( str != IntPtr.Zero ) Marshal.FreeCoTaskMem( str );
			}
		}

		public AtsResult Fail( ulong token, string reason )
		{
			using( var r = new Utf8( reason ?? string.Empty ) ) return (AtsResult)NativeMethods.ats_call_fail( Ptr, token, r.Ptr );
		}

		//---------------------------------------------------------------------
		// 共有変数
		//---------------------------------------------------------------------
		public T Get<T>( VarId<T> var ) => ValueConv<T>.From( GetValue( var.Id ) );
		public void Set<T>( VarId<T> var, T value ) => SetValue( var.Id, ValueConv<T>.To( value ) );

		public AtsValue GetValue( uint id )
		{
			Check( NativeMethods.ats_var_get( Ptr, id, out NativeValue n ), $"変数 {id} を読めません" );
			return AtsValue.FromNative( n );
		}

		public void SetValue( uint id, AtsValue value )
		{
			NativeValue n = value.ToNative( out IntPtr str );
			try {
				Check( NativeMethods.ats_var_set( Ptr, id, ref n ), $"変数 {id} に書けません" );
			} finally {
				if( str != IntPtr.Zero ) Marshal.FreeCoTaskMem( str );
			}
		}

		// 指定スクリプトの var をすべて初期値に戻す（ニューゲーム、クエスト破棄など）
		public void ResetScript( string scriptId )
		{
			using( var s = new Utf8( scriptId ) ) Check( NativeMethods.ats_script_reset( Ptr, s.Ptr ), $"スクリプト '{scriptId}' をリセットできません" );
		}

		//---------------------------------------------------------------------
		// セーブ（変数と乱数の状態だけ。実行中のファイバは保存しない）
		//---------------------------------------------------------------------
		public byte[] Save()
		{
			var ms = new MemoryStream();
			GCHandle h = GCHandle.Alloc( ms );
			try {
				Check( NativeMethods.ats_vm_save( Ptr, s_writePtr, GCHandle.ToIntPtr( h ) ), "セーブできません" );
			} finally {
				h.Free();
			}
			return ms.ToArray();
		}

		public void Load( byte[] data )
		{
			if( data == null ) throw new ArgumentNullException( nameof( data ) );
			var reader = new SaveReader { Data = data };
			GCHandle h = GCHandle.Alloc( reader );
			try {
				Check( NativeMethods.ats_vm_load( Ptr, s_readPtr, GCHandle.ToIntPtr( h ) ), "ロードできません" );
			} finally {
				h.Free();
			}
		}

		sealed class SaveReader
		{
			public byte[]	Data;
			public int		Pos;
		}

		static readonly WriteFn	s_writeFn	= OnWrite;
		static readonly ReadFn	s_readFn	= OnRead;
		static readonly IntPtr	s_writePtr	= Marshal.GetFunctionPointerForDelegate( s_writeFn );
		static readonly IntPtr	s_readPtr	= Marshal.GetFunctionPointerForDelegate( s_readFn );

		[MonoPInvokeCallback( typeof( WriteFn ) )]
		static int OnWrite( IntPtr data, UIntPtr size, IntPtr user )
		{
			try {
				var ms = (MemoryStream)GCHandle.FromIntPtr( user ).Target;
				var buf = new byte[ (int)size ];
				Marshal.Copy( data, buf, 0, buf.Length );
				ms.Write( buf, 0, buf.Length );
				return 0;
			} catch {
				return (int)AtsResult.OutOfMemory;
			}
		}

		[MonoPInvokeCallback( typeof( ReadFn ) )]
		static int OnRead( IntPtr data, UIntPtr size, IntPtr user )
		{
			try {
				var r = (SaveReader)GCHandle.FromIntPtr( user ).Target;
				int n = (int)size;
				if( r.Pos + n > r.Data.Length ) return (int)AtsResult.BadFormat;
				Marshal.Copy( r.Data, r.Pos, data, n );
				r.Pos += n;
				return 0;
			} catch {
				return (int)AtsResult.BadFormat;
			}
		}

		//---------------------------------------------------------------------
		void Check( int r, string what )
		{
			if( r == 0 ) return;
			string detail = m_handle.IsClosed ? null : Utf8.ToManaged( NativeMethods.ats_vm_last_error( m_ptr ) );
			throw new AtsException( (AtsResult)r, string.IsNullOrEmpty( detail ) ? what : $"{what}: {detail}" );
		}
	}

	//=========================================================================
	//	型 T と AtsValue の変換（VarId<T> 用。型ごとに 1 回だけ作る）
	//=========================================================================
	internal static class ValueConv<T>
	{
		public static readonly Func<AtsValue, T> From;
		public static readonly Func<T, AtsValue> To;

		static ValueConv()
		{
			Type t = typeof( T );
			if( t == typeof( int ) ){
				From = (Func<AtsValue, T>)(object)new Func<AtsValue, int>( v => v.AsInt );
				To   = (Func<T, AtsValue>)(object)new Func<int, AtsValue>( AtsValue.Int );
			} else if( t == typeof( bool ) ){
				From = (Func<AtsValue, T>)(object)new Func<AtsValue, bool>( v => v.AsBool );
				To   = (Func<T, AtsValue>)(object)new Func<bool, AtsValue>( AtsValue.Bool );
			} else if( t == typeof( float ) ){
				From = (Func<AtsValue, T>)(object)new Func<AtsValue, float>( v => v.AsFloat );
				To   = (Func<T, AtsValue>)(object)new Func<float, AtsValue>( AtsValue.Float );
			} else if( t == typeof( string ) ){
				From = (Func<AtsValue, T>)(object)new Func<AtsValue, string>( v => v.AsString );
				To   = (Func<T, AtsValue>)(object)new Func<string, AtsValue>( AtsValue.String );
			} else if( t == typeof( AtsHandle ) ){
				From = (Func<AtsValue, T>)(object)new Func<AtsValue, AtsHandle>( v => v.AsHandle );
				To   = (Func<T, AtsValue>)(object)new Func<AtsHandle, AtsValue>( AtsValue.Handle );
			} else if( t.IsEnum ){
				From = v => (T)Enum.ToObject( t, v.AsInt );
				To   = v => AtsValue.Enum( Convert.ToInt32( v ) );
			} else {
				From = _ => throw new NotSupportedException( $"AtomScript の変数に使えない型: {t}" );
				To   = _ => throw new NotSupportedException( $"AtomScript の変数に使えない型: {t}" );
			}
		}
	}
}
