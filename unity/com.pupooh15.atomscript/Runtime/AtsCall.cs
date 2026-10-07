//=========================================================================
//	AtsCall：コマンド・クエリのハンドラに渡される呼び出し
//	ハンドラから戻ると使えなくなる（使い回される）。後で完了させるときは
//	Await を使うか、GetToken で取ったトークンを ScriptVM.Complete / Fail に渡す。
//=========================================================================
using System;
using System.Runtime.InteropServices;
using System.Threading;
using AtomScript.Interop;
using UnityEngine;

namespace AtomScript
{
	public sealed class AtsCall
	{
		ScriptRuntime	m_rt;
		IntPtr			m_call;
		PendingCall		m_pending;		// CancellationToken か Await で作られる
		bool			m_awaiting;		// Await で非同期の完了待ちに入った

		internal AtsCall() {}

		internal void Begin( ScriptRuntime rt, IntPtr call ) { m_rt = rt; m_call = call; m_pending = null; m_awaiting = false; }
		internal void End() { m_rt = null; m_call = IntPtr.Zero; m_pending = null; m_awaiting = false; }

		IntPtr Call => m_call != IntPtr.Zero ? m_call : throw new InvalidOperationException( "AtsCall はハンドラの中でだけ使える" );

		//---------------------------------------------------------------------
		// 引数
		//---------------------------------------------------------------------
		public int			ArgCount				=> NativeMethods.ats_call_argc( Call );
		public int			ArgInt( int index )		=> NativeMethods.ats_arg_int( Call, index );
		public float		ArgFloat( int index )	=> NativeMethods.ats_arg_float( Call, index );
		public bool			ArgBool( int index )	=> NativeMethods.ats_arg_bool( Call, index ) != 0;
		public string		ArgString( int index )	=> Utf8.ToManaged( NativeMethods.ats_arg_string( Call, index ) );
		public AtsHandle	ArgHandle( int index )	=> new AtsHandle( NativeMethods.ats_arg_handle( Call, index ) );

		public FiberId	Fiber	=> new FiberId( NativeMethods.ats_call_fiber( Call ) );
		public ScriptVM	VM		=> m_rt.FindVM( NativeMethods.ats_call_vm( Call ) );

		//---------------------------------------------------------------------
		// 結果（クエリと、その場で完了するコマンド）
		//---------------------------------------------------------------------
		public void SetResult( AtsValue value )
		{
			NativeValue n = value.ToNative( out IntPtr str );
			try {
				int r = NativeMethods.ats_call_set_result( Call, ref n );
				if( r != 0 ) throw new AtsException( (AtsResult)r, "結果を設定できません" );
			} finally {
				if( str != IntPtr.Zero ) Marshal.FreeCoTaskMem( str );
			}
		}

		//---------------------------------------------------------------------
		// 待機ありコマンド
		//---------------------------------------------------------------------
		// ファイバが中断されたとき（abort・race・VM の破棄）にキャンセルされるトークン
		public CancellationToken CancellationToken => EnsurePending().Cancellation;

		// 低レベル API：Pending を返して、後で ScriptVM.Complete( token ) / Fail( token ) を呼ぶ
		public ulong GetToken() => NativeMethods.ats_call_get_token( Call );

		// Awaitable が終わったら完了を通知する。その場で終わっていれば Done を返す
		public AtsStatus Await( Awaitable awaitable )
		{
			if( awaitable == null || awaitable.IsCompleted ){
				awaitable?.GetAwaiter().GetResult();		// 例外ならそのまま投げる（失敗になる）
				return AtsStatus.Done;
			}
			PendingCall p = EnsurePending();
			m_awaiting = true;
			Drive( awaitable, p );
			return AtsStatus.Pending;
		}

		// 結果のあるコマンド。toValue で結果を AtsValue にする
		public AtsStatus Await<T>( Awaitable<T> awaitable, Func<T, AtsValue> toValue )
		{
			if( awaitable == null ) throw new ArgumentNullException( nameof( awaitable ) );
			var awaiter = awaitable.GetAwaiter();
			if( awaiter.IsCompleted ){
				SetResult( toValue( awaiter.GetResult() ) );
				return AtsStatus.Done;
			}
			PendingCall p = EnsurePending();
			m_awaiting = true;
			Drive( awaitable, toValue, p );
			return AtsStatus.Pending;
		}

		PendingCall EnsurePending()
		{
			if( m_pending == null ){
				IntPtr call = Call;
				m_pending = m_rt.AddPending( NativeMethods.ats_call_vm( call ), NativeMethods.ats_call_get_token( call ) );
			}
			return m_pending;
		}

		// ハンドラから戻ったとき。非同期の完了待ちでなければ待機情報を捨てる
		internal void Settle( AtsStatus status )
		{
			if( m_pending != null && !(m_awaiting && status == AtsStatus.Pending) ) m_pending.Release();
		}

		static async void Drive( Awaitable awaitable, PendingCall p )
		{
			try {
				await awaitable;
				p.Complete( null );
			} catch( OperationCanceledException ) when( p.IsCancelled ){
				// 中断済み
			} catch( Exception e ){
				p.Fail( e );
			}
		}

		static async void Drive<T>( Awaitable<T> awaitable, Func<T, AtsValue> toValue, PendingCall p )
		{
			try {
				T r = await awaitable;
				p.Complete( toValue( r ) );
			} catch( OperationCanceledException ) when( p.IsCancelled ){
				// 中断済み
			} catch( Exception e ){
				p.Fail( e );
			}
		}
	}

	//=========================================================================
	//	待機中の呼び出し 1 つ（VM とトークンの組で引く）
	//=========================================================================
	internal sealed class PendingCall
	{
		readonly ScriptRuntime				m_rt;
		readonly CancellationTokenSource	m_cts = new CancellationTokenSource();
		bool								m_done;

		public readonly IntPtr	VM;
		public readonly ulong	Token;

		public PendingCall( ScriptRuntime rt, IntPtr vm, ulong token ) { m_rt = rt; VM = vm; Token = token; }

		public CancellationToken	Cancellation	=> m_cts.Token;
		public bool					IsCancelled { get; private set; }

		// 結果を VM に伝える（中断済み・VM 破棄済みなら何もしない）
		public void Complete( AtsValue? result )
		{
			if( m_done ) return;
			Release();
			ScriptVM vm = m_rt.IsDisposed ? null : m_rt.FindVM( VM );
			if( vm == null ) return;
			AtsResult r = result.HasValue ? vm.Complete( Token, result.Value ) : vm.Complete( Token );
			if( r != AtsResult.Ok && r != AtsResult.StaleId )
				m_rt.Log?.Invoke( AtsLogLevel.Error, $"[AtomScript] 完了を通知できません（{r}）" );
		}

		public void Fail( Exception e )
		{
			if( m_done ) return;
			Release();
			ScriptVM vm = m_rt.IsDisposed ? null : m_rt.FindVM( VM );
			if( vm == null ) return;
			m_rt.ReportException( "await", e );
			vm.Fail( Token, e.Message );
		}

		// VM からのキャンセル通知
		public void Cancel()
		{
			if( m_done ) return;
			IsCancelled = true;
			Release();
			m_cts.Cancel();
		}

		public void Release()
		{
			m_done = true;
			m_rt.RemovePending( this );
		}
	}
}
